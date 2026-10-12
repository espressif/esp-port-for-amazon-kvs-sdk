#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
aiortc-based KVS WebRTC viewer.

Acts as the VIEWER side of a Kinesis Video Streams signaling channel:
- Looks up (or creates) the channel ARN.
- Resolves the WSS + HTTPS endpoints for the VIEWER role.
- Pulls TURN credentials via the signaling HTTPS endpoint.
- SigV4-presigns the WSS URL and opens the WebSocket.
- Builds an aiortc RTCPeerConnection with recvonly H.264 / Opus.
- Sends an SDP_OFFER and trickles local ICE candidates.
- Applies the SDP_ANSWER + remote ICE candidates.
- Records received tracks to OUT_PATH (MKV) and dumps raw H.264 to RAW_H264_PATH.
- Exits cleanly after TEST_DURATION_SEC.

It records what the SDK's master sends so the stream can be checked
(MKV + raw Annex-B dump).
"""

import asyncio
import base64
import json
import logging
import os
import sys
import tempfile
import time
import urllib.parse
import uuid

import boto3
from botocore.auth import SigV4QueryAuth
from botocore.awsrequest import AWSRequest
from aiortc import (
    RTCBundlePolicy,
    RTCConfiguration,
    RTCIceCandidate,
    RTCIceServer,
    RTCPeerConnection,
    RTCRtpCodecCapability,
    RTCSessionDescription,
)
from aiortc.contrib.media import MediaRecorder
from aiortc.codecs import h264 as _aiortc_h264
from aiortc.sdp import candidate_from_sdp
import websockets

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
log = logging.getLogger("python_viewer")
# aiortc and its sub-loggers are noisy at INFO — keep them at WARNING.
for noisy in ("aiortc", "aioice", "websockets"):
    logging.getLogger(noisy).setLevel(logging.WARNING)


REGION = os.environ.get("AWS_DEFAULT_REGION", "us-west-2")
CHANNEL = os.environ["KVS_CHANNEL_NAME"]
DURATION = int(os.environ.get("TEST_DURATION_SEC", "45"))
OUT_PATH = os.environ.get("OUT_PATH", f"{tempfile.gettempdir()}/python_viewer.mkv")
# Raw Annex-B dump of depacketized frames, captured before libavcodec so it
# is written even when decoding fails.
RAW_H264_PATH = os.environ.get(
    "RAW_H264_PATH",
    os.path.splitext(OUT_PATH)[0] + ".h264" if OUT_PATH
    else f"{tempfile.gettempdir()}/python_viewer.h264",
)
CLIENT_ID = os.environ.get("CLIENT_ID", f"py-viewer-{uuid.uuid4().hex[:8]}")


# Patch H264Decoder.decode (one Annex-B access unit per call) to dump each AU
# to RAW_H264_PATH, then hand it to the real decoder for MediaRecorder.
class _RawH264Sink:
    def __init__(self, path: str) -> None:
        self.path = path
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        self._fh = open(path, "wb")
        self.frames = 0
        self.bytes = 0

    def write(self, data: bytes) -> None:
        self._fh.write(data)
        self.frames += 1
        self.bytes += len(data)

    def close(self) -> None:
        try:
            self._fh.flush()
            self._fh.close()
        except Exception:
            pass


_raw_sink = _RawH264Sink(RAW_H264_PATH)
_orig_h264_decode = _aiortc_h264.H264Decoder.decode


def _has_idr_with_params(data: bytes) -> bool:
    """Return True if Annex-B `data` contains an SPS, a PPS and an IDR NAL."""
    have = {7: False, 8: False, 5: False}
    n = len(data)
    i = 0
    while i < n - 2:
        if data[i] == 0 and data[i + 1] == 0:
            if i + 3 < n and data[i + 2] == 0 and data[i + 3] == 1:
                t = data[i + 4] & 0x1F if i + 4 < n else 0
                i += 4
            elif data[i + 2] == 1:
                t = data[i + 3] & 0x1F if i + 3 < n else 0
                i += 3
            else:
                i += 1
                continue
            if t in have:
                have[t] = True
                if all(have.values()):
                    return True
            continue
        i += 1
    return False


_seen_idr_params = False


def _decode_with_dump(self, encoded_frame):  # type: ignore[no-untyped-def]
    global _seen_idr_params
    data = getattr(encoded_frame, "data", None)
    if data:
        try:
            _raw_sink.write(bytes(data))
        except Exception as exc:  # never let the dump break decoding
            log.warning("raw h264 dump write failed: %s", exc)
        if not _seen_idr_params:
            if _has_idr_with_params(bytes(data)):
                _seen_idr_params = True
                log.info("H264Decoder: first SPS+PPS+IDR seen — enabling libavcodec from this AU")
            else:
                # Hold libavcodec until SPS+PPS+IDR arrive; aiortc's decoder
                # does not recover from pre-IDR slices.
                return []
    return _orig_h264_decode(self, encoded_frame)


_aiortc_h264.H264Decoder.decode = _decode_with_dump  # type: ignore[assignment]


def get_channel_arn(kvs) -> str:
    """Look up the channel ARN; create the channel if it doesn't exist yet."""
    try:
        return kvs.describe_signaling_channel(ChannelName=CHANNEL)["ChannelInfo"]["ChannelARN"]
    except kvs.exceptions.ResourceNotFoundException:
        log.info("Channel %s not found — creating", CHANNEL)
        kvs.create_signaling_channel(ChannelName=CHANNEL)
        return kvs.describe_signaling_channel(ChannelName=CHANNEL)["ChannelInfo"]["ChannelARN"]


def resolve_endpoints(kvs, arn: str) -> dict:
    resp = kvs.get_signaling_channel_endpoint(
        ChannelARN=arn,
        SingleMasterChannelEndpointConfiguration={
            "Protocols": ["WSS", "HTTPS"],
            "Role": "VIEWER",
        },
    )
    return {e["Protocol"]: e["ResourceEndpoint"] for e in resp["ResourceEndpointList"]}


def build_ice_servers(https_endpoint: str, arn: str) -> list[RTCIceServer]:
    signaling = boto3.client(
        "kinesis-video-signaling", region_name=REGION, endpoint_url=https_endpoint
    )
    resp = signaling.get_ice_server_config(ChannelARN=arn, ClientId=CLIENT_ID)
    servers = [RTCIceServer(urls=[f"stun:stun.kinesisvideo.{REGION}.amazonaws.com:443"])]
    for s in resp["IceServerList"]:
        servers.append(
            RTCIceServer(urls=s["Uris"], username=s["Username"], credential=s["Password"])
        )
    log.info("Got %d TURN server(s) from KVS", len(resp["IceServerList"]))
    return servers


def sigv4_presign_wss(wss_endpoint: str, arn: str) -> str:
    """SigV4 query-string presign on the WSS URL for the kinesisvideo service."""
    creds = boto3.Session().get_credentials().get_frozen_credentials()
    params = {"X-Amz-ChannelARN": arn, "X-Amz-ClientId": CLIENT_ID}
    url = wss_endpoint + "?" + urllib.parse.urlencode(params)
    request = AWSRequest(method="GET", url=url)
    SigV4QueryAuth(creds, "kinesisvideo", REGION).add_auth(request)
    return request.url


async def run() -> int:
    log.info("region=%s channel=%s client_id=%s duration=%ds out=%s",
             REGION, CHANNEL, CLIENT_ID, DURATION, OUT_PATH)

    # KVS may reject some STUN/TURN requests (e.g. "403 Forbidden IP"); other
    # candidates can still succeed, so log these instead of letting them escape.
    def loop_exception_handler(loop, context):
        exc = context.get("exception")
        if exc is not None:
            etype = type(exc).__name__
            estr = str(exc)
            if (
                "TransactionFailed" in etype
                or "TURN" in estr
                or "STUN" in estr
                or "Forbidden IP" in estr
                or " 403 " in f" {estr} "
            ):
                log.warning("Suppressing non-fatal TURN/STUN error: %s: %s", etype, estr)
                return
        log.warning("Unhandled async error: %s", context.get("message", "?"))

    asyncio.get_running_loop().set_exception_handler(loop_exception_handler)

    pc = None
    recorder = None
    recorder_started = False
    track_count = 0
    status_errors = 0

    try:
        kvs = boto3.client("kinesisvideo", region_name=REGION)
        arn = get_channel_arn(kvs)
        endpoints = resolve_endpoints(kvs, arn)
        ice_servers = build_ice_servers(endpoints["HTTPS"], arn)
        signed_wss = sigv4_presign_wss(endpoints["WSS"], arn)

        # KVS signs all STUN checks with one ufrag/pwd; MAX_BUNDLE matches that.
        pc = RTCPeerConnection(RTCConfiguration(
            iceServers=ice_servers,
            bundlePolicy=RTCBundlePolicy.MAX_BUNDLE,
        ))
        recorder = MediaRecorder(OUT_PATH)

        # Offer only H.264 + Opus; the master rejects offers led by other codecs.
        video_caps = [
            RTCRtpCodecCapability(mimeType="video/H264", clockRate=90000,
                                  parameters={
                                      "level-asymmetry-allowed": "1",
                                      "packetization-mode": "1",
                                      "profile-level-id": "42e01f",
                                  }),
        ]
        audio_caps = [
            RTCRtpCodecCapability(mimeType="audio/opus", clockRate=48000, channels=2),
        ]
        video_t = pc.addTransceiver("video", direction="recvonly")
        video_t.setCodecPreferences(video_caps)
        audio_t = pc.addTransceiver("audio", direction="recvonly")
        audio_t.setCodecPreferences(audio_caps)

        @pc.on("track")
        def on_track(track):
            nonlocal track_count
            track_count += 1
            log.info("Track received: kind=%s id=%s", track.kind, track.id)
            recorder.addTrack(track)

        @pc.on("connectionstatechange")
        async def on_state():
            log.info("PC connection state: %s", pc.connectionState)

        @pc.on("iceconnectionstatechange")
        async def on_ice_state():
            log.info("ICE connection state: %s", pc.iceConnectionState)

        # Advertise trickle ICE so the master answers without waiting for full gathering.
        offer = await pc.createOffer()
        if "a=ice-options:trickle" not in offer.sdp:
            sep = "\r\n" if "\r\n" in offer.sdp else "\n"
            sdp_lines = offer.sdp.split(sep)
            insert_idx = next(
                (i for i, line in enumerate(sdp_lines) if line.startswith("m=")),
                len(sdp_lines),
            )
            sdp_lines.insert(insert_idx, "a=ice-options:trickle")
            offer = RTCSessionDescription(sdp=sep.join(sdp_lines), type=offer.type)

        await pc.setLocalDescription(offer)

        # Wait briefly for gathering, then trickle whatever is in localDescription.
        deadline_gather = time.monotonic() + 2.0
        while pc.iceGatheringState != "complete" and time.monotonic() < deadline_gather:
            await asyncio.sleep(0.05)
        log.info("ICE gathering state=%s; opening WSS to %s",
                 pc.iceGatheringState, endpoints["WSS"])

        async with websockets.connect(signed_wss, max_size=2**20) as ws:
            offer_payload = {"type": pc.localDescription.type, "sdp": pc.localDescription.sdp}
            await ws.send(json.dumps({
                "action": "SDP_OFFER",
                "messagePayload": base64.b64encode(json.dumps(offer_payload).encode()).decode(),
            }))
            log.info("Sent SDP_OFFER to master")

            # aiortc puts gathered candidates inline in the SDP; send each as ICE_CANDIDATE.
            local_sdp = pc.localDescription.sdp
            sep = "\r\n" if "\r\n" in local_sdp else "\n"
            current_mid = None
            mline_index = -1
            sent_candidates = 0
            for line in local_sdp.split(sep):
                if line.startswith("m="):
                    mline_index += 1
                    current_mid = None
                elif line.startswith("a=mid:"):
                    current_mid = line.split(":", 1)[1].strip()
                elif line.startswith("a=candidate:"):
                    cand_str = line[2:]  # strip "a="
                    payload = {
                        "candidate": cand_str,
                        "sdpMid": current_mid if current_mid is not None else str(max(mline_index, 0)),
                        "sdpMLineIndex": max(mline_index, 0),
                    }
                    await ws.send(json.dumps({
                        "action": "ICE_CANDIDATE",
                        "messagePayload": base64.b64encode(json.dumps(payload).encode()).decode(),
                        "correlationId": uuid.uuid4().hex,
                        "recipientClientId": "MASTER",
                    }))
                    sent_candidates += 1
            log.info("Sent %d trickled ICE candidate(s) to master", sent_candidates)

            await recorder.start()
            recorder_started = True
            deadline = time.monotonic() + DURATION

            async def pump_signaling():
                nonlocal status_errors
                async for raw in ws:
                    # Skip keepalives and other non-JSON frames.
                    try:
                        msg = json.loads(raw)
                    except (json.JSONDecodeError, TypeError):
                        log.debug("Non-JSON ws frame, len=%d", len(raw) if raw else 0)
                        continue
                    if not isinstance(msg, dict):
                        continue
                    mtype = (msg.get("messageType") or "").upper()

                    # STATUS_RESPONSE has no messagePayload; handle it first.
                    if mtype == "STATUS_RESPONSE":
                        status = msg.get("statusResponse") or msg
                        code = status.get("statusCode")
                        if code is not None and str(code) != "200":
                            status_errors += 1
                            log.error(
                                "STATUS_RESPONSE error: statusCode=%s errorType=%s "
                                "description=%s correlationId=%s",
                                code, status.get("errorType"), status.get("description"),
                                status.get("correlationId"),
                            )
                        else:
                            log.info("STATUS_RESPONSE: %s", status)
                        continue

                    payload_b64 = msg.get("messagePayload") or ""
                    if not payload_b64:
                        continue
                    try:
                        payload = base64.b64decode(payload_b64).decode()
                    except Exception:
                        log.warning("Could not decode payload for %s", mtype)
                        continue

                    if mtype == "SDP_ANSWER":
                        answer = json.loads(payload)
                        await pc.setRemoteDescription(
                            RTCSessionDescription(sdp=answer["sdp"], type=answer["type"])
                        )
                        log.info("Applied SDP_ANSWER (sdp=%d bytes)", len(answer["sdp"]))
                    elif mtype == "ICE_CANDIDATE":
                        cand_obj = json.loads(payload)
                        cand_str = cand_obj.get("candidate") or ""
                        _, has_colon, cand_body = cand_str.partition(":")
                        if not has_colon or not cand_body:
                            log.warning("Ignoring malformed remote ICE candidate: %r", cand_str)
                            continue
                        try:
                            candidate = candidate_from_sdp(cand_body)
                        except Exception as exc:
                            log.warning("Could not parse remote ICE candidate %r: %s", cand_str, exc)
                            continue
                        candidate.sdpMid = cand_obj.get("sdpMid")
                        candidate.sdpMLineIndex = cand_obj.get("sdpMLineIndex")
                        await pc.addIceCandidate(candidate)
                        log.info("Added remote ICE candidate")
                    else:
                        log.debug("Ignored signaling message type=%r", mtype)

            signaling_task = asyncio.create_task(pump_signaling())
            try:
                await asyncio.wait_for(asyncio.shield(signaling_task),
                                       timeout=max(1.0, deadline - time.monotonic()))
            except asyncio.TimeoutError:
                log.info("Reached test duration (%ds), stopping", DURATION)
            finally:
                signaling_task.cancel()
    finally:
        if recorder is not None and recorder_started:
            try:
                await recorder.stop()
            except Exception as exc:
                log.warning("recorder.stop() failed: %s", exc)
        if pc is not None:
            try:
                await pc.close()
            except Exception as exc:
                log.warning("pc.close() failed: %s", exc)
        _raw_sink.close()

    raw_size = os.path.getsize(RAW_H264_PATH) if os.path.exists(RAW_H264_PATH) else 0
    mkv_size = os.path.getsize(OUT_PATH) if os.path.exists(OUT_PATH) else 0
    log.info(
        "Recording closed. Tracks observed: %d. MKV: %s (%d bytes). Raw H.264: %s (%d frames, %d bytes).",
        track_count, OUT_PATH, mkv_size, RAW_H264_PATH, _raw_sink.frames, _raw_sink.bytes,
    )
    if status_errors:
        log.error("Signaling reported %d STATUS_RESPONSE error(s)", status_errors)
    if track_count == 0:
        log.error("No tracks ever arrived — viewer failed")
        return 1
    # Pass if either the MKV or the raw dump (written even when libavcodec fails) grew.
    raw_ok = _raw_sink.frames >= 5 and raw_size >= 1024
    mkv_ok = mkv_size >= 1024
    if not raw_ok and not mkv_ok:
        log.error("Neither the MKV nor the raw H.264 dump grew — viewer failed")
        return 2
    log.info("python_viewer: PASS (raw=%s, mkv=%s)", "ok" if raw_ok else "empty",
             "ok" if mkv_ok else "empty")
    return 0


if __name__ == "__main__":
    try:
        rc = asyncio.run(run())
    except KeyboardInterrupt:
        rc = 130
    sys.exit(rc)
