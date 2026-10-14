# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""aiortc-based KVS WebRTC viewer.

Joins a Kinesis Video Streams signaling channel as VIEWER, offers to the device
(which runs as MASTER), and counts the media frames that come back.

Importable by the e2e pytest, or runnable by hand:

    python kvs_viewer.py --channel ESP32Channel --region us-east-1 --duration 20
"""

import argparse
import asyncio
import base64
import json
import logging
import time
import uuid
from dataclasses import dataclass, field
from urllib.parse import urlencode, urlparse

import boto3
import websockets
from botocore.auth import SigV4QueryAuth
from botocore.awsrequest import AWSRequest
from aiortc import RTCConfiguration, RTCIceServer, RTCPeerConnection, RTCSessionDescription
from aiortc.sdp import candidate_from_sdp

logger = logging.getLogger('kvs_viewer')

# The presigned WSS URL is only valid for this long; KVS caps it at 300 s.
_PRESIGN_EXPIRY_S = 299

# Upper bound on each teardown step. aiortc's close() deletes the TURN allocation
# and aioice keeps retrying that once the peer is gone, so an unbounded close() can
# outlive the run by minutes and the result line is never printed.
_TEARDOWN_TIMEOUT_S = 10.0


@dataclass
class ViewerResult:
    """What the test asserts on."""
    client_id: str
    frames_video: int = 0
    frames_audio: int = 0
    first_video_latency_s: float = None
    first_audio_latency_s: float = None
    ice_state: str = 'new'
    connection_state: str = 'new'
    local_cand_type: str = None
    remote_cand_type: str = None
    answer_received: bool = False
    remote_candidates: int = 0
    dc_opened: bool = False
    dc_echo: str = None
    video_packets: int = 0
    audio_packets: int = 0
    video_packets_lost: int = 0
    video_rtx_packets: int = 0
    errors: list = field(default_factory=list)

    @property
    def video_fps(self):
        if not self.frames_video or self.first_video_latency_s is None:
            return 0.0
        window = self.media_window_s
        return self.frames_video / window if window > 0 else 0.0

    media_window_s: float = 0.0

    def summary(self):
        return (
            f'client_id={self.client_id} answer={self.answer_received} '
            f'ice={self.ice_state} pc={self.connection_state} '
            f'video_pkts={self.video_packets} audio_pkts={self.audio_packets} '
            f'video_lost={self.video_packets_lost} video_rtx={self.video_rtx_packets} '
            f'video_frames={self.frames_video} audio_frames={self.frames_audio} '
            f'video_fps={self.video_fps:.1f} '
            f'first_video={self.first_video_latency_s} '
            f'dc_open={self.dc_opened} dc_echo={self.dc_echo!r} '
            f'pair={self.local_cand_type}/{self.remote_cand_type} '
            f'remote_cands={self.remote_candidates} errors={self.errors}'
        )


class KvsViewer:
    """One viewer session against one signaling channel."""

    def __init__(self, channel_name, region, credentials=None, client_id=None,
                 duration_s=20.0, connect_timeout_s=60.0, want_audio=True,
                 want_data_channel=False):
        self.channel_name = channel_name
        self.region = region
        # boto3 Session resolves the ambient credential chain when None.
        self._session = boto3.Session(region_name=region, **(credentials or {}))
        self.client_id = client_id or f'pytest-viewer-{uuid.uuid4().hex[:8]}'
        self.duration_s = duration_s
        self.connect_timeout_s = connect_timeout_s
        # Set when the peer connection comes up, so duration_s is the media window.
        self._media_deadline = None
        self._started = time.monotonic()
        self.want_audio = want_audio
        self.want_data_channel = want_data_channel
        self.result = ViewerResult(client_id=self.client_id)
        self._pc = None
        self._ws = None
        self._tracks_done = None
        self._stop_sampling = asyncio.Event()
        # The master trickles ICE_CANDIDATEs before its SDP_ANSWER lands; aiortc
        # drops candidates added without a remote description, so hold them.
        self._early_candidates = []


    def _describe(self):
        """Resolve channel ARN, the VIEWER WSS/HTTPS endpoints, and ICE servers."""
        kv = self._session.client('kinesisvideo')
        arn = kv.describe_signaling_channel(
            ChannelName=self.channel_name)['ChannelInfo']['ChannelARN']

        endpoints = kv.get_signaling_channel_endpoint(
            ChannelARN=arn,
            SingleMasterChannelEndpointConfiguration={
                'Protocols': ['WSS', 'HTTPS'],
                'Role': 'VIEWER',
            },
        )['ResourceEndpointList']
        by_proto = {e['Protocol']: e['ResourceEndpoint'] for e in endpoints}

        ice = self._session.client(
            'kinesis-video-signaling', endpoint_url=by_proto['HTTPS'],
        ).get_ice_server_config(ChannelARN=arn)['IceServerList']

        # KVS's own STUN server is not in IceServerList; add it as the SDK does.
        servers = [RTCIceServer(urls=f'stun:stun.kinesisvideo.{self.region}.amazonaws.com:443')]
        for s in ice:
            servers.append(RTCIceServer(urls=s['Uris'], username=s['Username'],
                                        credential=s['Password']))
        logger.info('channel=%s arn=%s ice_servers=%d', self.channel_name, arn, len(servers))
        return arn, by_proto['WSS'], servers

    def _presign_wss(self, wss_endpoint, arn):
        """SigV4-presign the signaling GET. Query auth, not header auth -- a WebSocket
        handshake has nowhere to put an Authorization header."""
        creds = self._session.get_credentials()
        if creds is None:
            raise RuntimeError('no AWS credentials resolved')
        query = urlencode({'X-Amz-ChannelARN': arn, 'X-Amz-ClientId': self.client_id})
        request = AWSRequest(method='GET', url=f'{wss_endpoint}/?{query}')
        SigV4QueryAuth(creds.get_frozen_credentials(), 'kinesisvideo', self.region,
                       expires=_PRESIGN_EXPIRY_S).add_auth(request)
        return request.url


    async def _send(self, action, payload, recipient=None):
        msg = {
            'action': action,
            'messagePayload': base64.b64encode(json.dumps(payload).encode()).decode(),
        }
        # A viewer's messages route to the master implicitly.
        if recipient:
            msg['recipientClientId'] = recipient
        await self._ws.send(json.dumps(msg))
        logger.debug('-> %s', action)

    async def _handle(self, raw):
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            logger.debug('undecodable signaling frame: %r', raw[:200])  # keepalives
            return
        mtype = msg.get('messageType')
        if mtype == 'STATUS_RESPONSE':
            # KVS reports per-message delivery status here; only surface failures.
            body = msg.get('statusResponse', {})
            if body.get('success') is False:
                self.result.errors.append(f'status_response: {body}')
                logger.error('signaling rejected a message: %s', body)
            return
        try:
            payload = json.loads(base64.b64decode(msg['messagePayload']))
        except (KeyError, ValueError) as exc:
            logger.warning('bad messagePayload on %s: %s', mtype, exc)
            return

        if mtype == 'SDP_ANSWER':
            logger.info('<- SDP_ANSWER from %s', msg.get('senderClientId'))
            await self._pc.setRemoteDescription(
                RTCSessionDescription(sdp=payload['sdp'], type='answer'))
            self.result.answer_received = True
            for cand in self._early_candidates:
                await self._pc.addIceCandidate(cand)
                self.result.remote_candidates += 1
            self._early_candidates.clear()
        elif mtype == 'ICE_CANDIDATE':
            if not payload.get('candidate'):
                return  # end-of-candidates marker
            cand = self._parse_candidate(payload)
            if not self.result.answer_received:
                self._early_candidates.append(cand)
                return
            await self._pc.addIceCandidate(cand)
            self.result.remote_candidates += 1
        else:
            logger.debug('<- ignoring %s', mtype)

    @staticmethod
    def _parse_candidate(payload):
        cand = candidate_from_sdp(payload['candidate'].replace('candidate:', '', 1))
        cand.sdpMid = payload.get('sdpMid')
        idx = payload.get('sdpMLineIndex')
        cand.sdpMLineIndex = int(idx) if idx is not None else None
        return cand


    def _wire_pc(self, servers):
        pc = RTCPeerConnection(RTCConfiguration(iceServers=servers))
        self._pc = pc
        counts = {'video': 0, 'audio': 0}
        pending = set()

        @pc.on('iceconnectionstatechange')
        async def _ice():
            self.result.ice_state = pc.iceConnectionState
            logger.info('ice -> %s', pc.iceConnectionState)

        @pc.on('connectionstatechange')
        async def _conn():
            self.result.connection_state = pc.connectionState
            logger.info('pc -> %s', pc.connectionState)
            if pc.connectionState == 'connected' and self._media_deadline is None:
                self._media_deadline = time.monotonic() + self.duration_s

        @pc.on('track')
        def _track(track):
            logger.info('track: %s', track.kind)
            pending.add(asyncio.ensure_future(self._drain(track, counts)))

        if self.want_data_channel:
            self._wire_data_channel(pc)

        self._tracks_done = (pending, counts)
        return pc

    def _wire_data_channel(self, pc):
        """Open a data channel and, if anything answers, record the reply.

        The channel reaching 'open' is the assertion that matters: it means the SCTP
        association came up over DTLS, which is a distinct transport from the RTP path
        and can regress on its own. An echo only arrives if the firmware registered a
        handler -- webrtc_classic currently does not -- so dc_echo stays None there
        without failing anything.
        """
        channel = pc.createDataChannel('e2e-test')

        @channel.on('open')
        def _open():
            self.result.dc_opened = True
            logger.info('data channel open')
            channel.send('ping-from-pytest')

        @channel.on('message')
        def _message(msg):
            text = msg if isinstance(msg, str) else msg.decode('utf-8', 'replace')
            self.result.dc_echo = text
            logger.info('data channel echo: %r', text)

    def _deadline(self):
        """Connecting is bounded by connect_timeout_s; once connected, the window is
        duration_s from that point."""
        if self._media_deadline is not None:
            return self._media_deadline
        return self._started + self.connect_timeout_s

    async def _drain(self, track, counts):
        """Pull frames until the media window closes or the track ends."""
        while True:
            left = self._deadline() - time.monotonic()
            if left <= 0:
                break
            try:
                await asyncio.wait_for(track.recv(), timeout=max(1.0, left))
            except asyncio.TimeoutError:
                break
            except Exception as exc:  # MediaStreamError on track end
                logger.info('%s track ended: %s', track.kind, type(exc).__name__)
                break
            counts[track.kind] += 1
            # Media is flowing, so start the window here too: a run that streamed
            # fine but never latched 'connected' ran ~90 s instead of 20 s.
            if self._media_deadline is None:
                self._media_deadline = time.monotonic() + self.duration_s
            if counts[track.kind] == 1:
                latency = time.monotonic() - self._started
                if track.kind == 'video':
                    self.result.first_video_latency_s = round(latency, 2)
                else:
                    self.result.first_audio_latency_s = round(latency, 2)
                logger.info('first %s frame at %.2fs', track.kind, latency)

    def _snapshot_pair(self):
        """Best-effort read of the nominated candidate pair.

        aiortc exposes no candidate-pair stats, so this digs through aioice internals
        and is deliberately non-fatal -- the device-side 'Selected pair' log is the
        authoritative P2P oracle.
        """
        try:
            for transceiver in self._pc.getTransceivers():
                ice = transceiver.receiver.transport.transport._connection
                for pair in getattr(ice, 'check_list', []):
                    if getattr(pair, 'nominated', False):
                        self.result.local_cand_type = pair.local_candidate.type
                        self.result.remote_cand_type = pair.remote_candidate.type
                        return
        except Exception as exc:
            logger.debug('could not read candidate pair: %s', exc)

    @staticmethod
    def _offer_sdp(pc):
        """Advertise trickle ICE even though aiortc gathers up front.

        Without a=ice-options:trickle the KVS master mirrors non-trickle mode and
        withholds its answer until *its* gathering completes -- which means waiting out
        every TURN allocation retry (observed at 20 s against an unreachable relay).
        Declaring trickle gets the answer immediately; the master's own candidates then
        arrive as ICE_CANDIDATE messages, which _handle() already applies. Our own
        candidates are all in this SDP regardless, so we never have to trickle out.
        """
        lines = pc.localDescription.sdp.split('\r\n')

        # aiortc gives every m-line its own ice-ufrag/ice-pwd even while offering
        # a=group:BUNDLE. The KVS master signs its connectivity checks with the
        # credentials of the *last* m-line it parses, but bundles onto the *first*
        # m-line's transport -- so every check it sends carries a password no
        # surviving connection holds, aioice answers 400 Bad Request, and ICE fails
        # to nominate even though the host pair works in the other direction.
        # One credential set across the group is what BUNDLE means anyway, and makes
        # the master right whichever m-line it read.
        first = {}
        for i, line in enumerate(lines):
            for attr in ('ice-ufrag', 'ice-pwd'):
                if line.startswith('a=' + attr + ':'):
                    if attr in first:
                        lines[i] = first[attr]
                    else:
                        first[attr] = line

        if 'a=ice-options:trickle' not in lines:
            # Session-level attribute: after the o=/s=/t= block, before the first m=.
            for i, line in enumerate(lines):
                if line.startswith('m='):
                    lines.insert(i, 'a=ice-options:trickle')
                    break
        return '\r\n'.join(lines)

    async def _sample_stats(self):
        """Poll inbound RTP counters until the media window closes.

        Sampling once at the end is wrong: aiortc's counters are per-SSRC and the
        report at close time can describe a stream that only just started, which
        produced nonsense like "3 packets received, 375 frames decoded". Keeping the
        running maximum per kind survives an SSRC change and a closing transport.
        """
        while not self._stop_sampling.is_set():
            try:
                await asyncio.wait_for(self._stop_sampling.wait(), timeout=2.0)
            except asyncio.TimeoutError:
                pass
            await self._snapshot_stats()

    async def _snapshot_stats(self):
        """Record inbound RTP counters, per kind, excluding retransmissions.

        Packet counts, not decoded frames, are the evidence that media flowed:
        aiortc's H.264 decoder rejects some valid streams, so frames can sit at zero
        while RTP arrives at full rate.
        """
        counts = self._receiver_counts()
        if counts is None:
            counts = await self._report_counts()
        # Max, not sum: this runs repeatedly over the same streams.
        v_got, v_lost, v_rtx = counts.get('video', (0, 0, 0))
        a_got = counts.get('audio', (0, 0, 0))[0]
        self.result.video_packets = max(self.result.video_packets, v_got)
        self.result.video_packets_lost = max(self.result.video_packets_lost, v_lost)
        self.result.video_rtx_packets = max(self.result.video_rtx_packets, v_rtx)
        self.result.audio_packets = max(self.result.audio_packets, a_got)

    def _receiver_counts(self):
        """{kind: (packets, lost, rtx_packets)} straight from each receiver's SSRCs.

        getStats() cannot be used: aiortc gives every inbound-rtp entry of a
        receiver the same id, so once the device retransmits on an RTX SSRC that
        entry replaces the media stream's -- 126 RTX packets reported against 2957
        real ones. None if aiortc's internals change shape.
        """
        out = {}
        for rx in self._pc.getReceivers():
            streams = getattr(rx, '_RTCRtpReceiver__remote_streams', None)
            rtx = getattr(rx, '_RTCRtpReceiver__rtx_ssrc', None)
            if streams is None or rtx is None or rx.track is None:
                return None
            got, lost, rtx_got = out.get(rx.track.kind, (0, 0, 0))
            for ssrc, st in streams.items():
                if ssrc in rtx:
                    rtx_got += st.packets_received
                else:
                    got += st.packets_received
                    lost += max(0, st.packets_lost)
            out[rx.track.kind] = (got, lost, rtx_got)
        return out

    async def _report_counts(self):
        """Fallback via getStats(); wrong once RTX starts (see _receiver_counts)."""
        try:
            report = await self._pc.getStats()
        except Exception as exc:
            logger.debug('getStats failed: %s', exc)
            return {}
        out = {}
        for stat in report.values():
            if getattr(stat, 'type', None) != 'inbound-rtp':
                continue
            got, lost, _ = out.get(stat.kind, (0, 0, 0))
            out[stat.kind] = (got + stat.packetsReceived, lost + max(0, stat.packetsLost), 0)
        return out


    async def _bounded(self, what, awaitable, timeout=_TEARDOWN_TIMEOUT_S):
        """Await one teardown step for at most `timeout` seconds.

        A step that hangs or raises is recorded in the result and skipped, so the
        measurements already taken still reach the caller.
        """
        try:
            return await asyncio.wait_for(awaitable, timeout=timeout)
        except asyncio.TimeoutError:
            logger.warning('%s did not finish in %.0f s; abandoning it', what, timeout)
            self.result.errors.append(f'{what} timed out')
        except Exception as exc:
            logger.warning('%s failed: %r', what, exc)
            self.result.errors.append(f'{what} failed: {type(exc).__name__}')
        return None

    def _ws_headers(self):
        """Extra headers for the signaling WebSocket handshake (none for KVS)."""
        return None

    async def _negotiate(self, pc):
        """Open the session: the viewer offers, as a KVS viewer does.

        aiortc has no trickle-out: setLocalDescription blocks until gathering
        finishes, so the offer we send already carries every local candidate."""
        await pc.setLocalDescription(await pc.createOffer())
        await self._send('SDP_OFFER',
                         {'type': 'offer', 'sdp': self._offer_sdp(pc)})

    async def run(self):
        arn, wss, servers = self._describe()
        url = self._presign_wss(wss, arn)
        pc = self._wire_pc(servers)

        # recvonly: v1 only proves device -> viewer streaming.
        pc.addTransceiver('video', direction='recvonly')
        if self.want_audio:
            pc.addTransceiver('audio', direction='recvonly')

        self._started = time.monotonic()
        sampler = asyncio.ensure_future(self._sample_stats())
        try:
            async with websockets.connect(url, max_size=None, close_timeout=5,
                                          additional_headers=self._ws_headers()) as ws:
                self._ws = ws
                logger.info('signaling connected as viewer %s', self.client_id)

                await self._negotiate(pc)

                while True:
                    left = self._deadline() - time.monotonic()
                    if left <= 0:
                        break
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=max(0.5, left))
                    except asyncio.TimeoutError:
                        break
                    except websockets.ConnectionClosed as exc:
                        self.result.errors.append(f'signaling closed: {exc}')
                        break
                    await self._handle(raw)
                    # Once media is flowing there is nothing left to signal; let the
                    # drain tasks own the rest of the window.
                    if self.result.answer_received and pc.connectionState == 'connected':
                        remaining = self._deadline() - time.monotonic()
                        if remaining > 0:
                            await asyncio.sleep(remaining)
                        break
        finally:
            # Every step is bounded: a run that measured media must still report it
            # when the peer has vanished and teardown cannot complete cleanly.
            pending, counts = self._tracks_done
            for task in list(pending):
                task.cancel()
            await self._bounded('media drain', asyncio.gather(*pending, return_exceptions=True))
            self._stop_sampling.set()
            await self._bounded('stats sampler', asyncio.gather(sampler, return_exceptions=True))
            await self._bounded('stats snapshot', self._snapshot_stats())
            self._snapshot_pair()
            self.result.frames_video = counts['video']
            self.result.frames_audio = counts['audio']
            if self.result.first_video_latency_s is not None:
                self.result.media_window_s = max(
                    0.001,
                    (time.monotonic() - self._started) - self.result.first_video_latency_s)
            await self._bounded('peer connection close', pc.close())
            if not self.result.answer_received:
                self.result.errors.append('no SDP_ANSWER received')
            logger.info('result: %s', self.result.summary())

        return self.result


def run_viewer(viewer, cleanup_timeout=_TEARDOWN_TIMEOUT_S):
    """Run one viewer session to completion and return its ViewerResult.

    Use this rather than asyncio.run(viewer.run()). asyncio.run() cancels every
    task left on the loop and then waits for all of them without a limit; aioice's
    TURN refresh and retransmit tasks can outlive a closed peer connection, so a
    run that already has its result would never return it. Here leftovers get
    `cleanup_timeout` seconds and are then abandoned with the loop.
    """
    loop = asyncio.new_event_loop()
    try:
        asyncio.set_event_loop(loop)
        result = loop.run_until_complete(viewer.run())
        leftovers = [t for t in asyncio.all_tasks(loop) if not t.done()]
        for task in leftovers:
            task.cancel()
        if leftovers:
            _, stuck = loop.run_until_complete(asyncio.wait(leftovers, timeout=cleanup_timeout))
            if stuck:
                logger.warning('abandoning %d task(s) still running after teardown', len(stuck))
        return result
    finally:
        asyncio.set_event_loop(None)
        loop.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--channel', required=True)
    ap.add_argument('--region', default='us-east-1')
    ap.add_argument('--duration', type=float, default=20.0,
                    help='media window in seconds, after connect')
    ap.add_argument('--connect-timeout', type=float, default=60.0)
    ap.add_argument('--no-audio', action='store_true')
    ap.add_argument('--data-channel', action='store_true',
                    help='also open a data channel and report whether it came up')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format='%(asctime)s %(name)s %(levelname)s %(message)s')

    viewer = KvsViewer(args.channel, args.region, duration_s=args.duration,
                       connect_timeout_s=args.connect_timeout,
                       want_audio=not args.no_audio,
                       want_data_channel=args.data_channel)
    result = run_viewer(viewer)
    print(result.summary(), flush=True)
    return 0 if result.video_packets > 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
