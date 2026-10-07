# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""AppRTC viewer: the same peer connection as kvs_viewer, signed into a room
instead of an AWS channel.

Only the signaling plane differs. Everything the test asserts on -- RTP arriving,
packet loss, the selected candidate pair, the data channel, the stats snapshot --
comes from KvsViewer unchanged, so a result from here is comparable with a result
from there.

What this buys: no AWS account, no SigV4, and no session token that expires in the
middle of a campaign. What it cannot cover: the KVS signaling state walk, which is
KVS's own enum and has no counterpart here. That assertion stays in
pytest_webrtc_classic.py.

The device side is examples/esp_camera with CONFIG_APPRTC_USE_FIXED_ROOM=y and
CONFIG_APPRTC_ROOM_ID set to the room passed here.

    python apprtc_viewer.py --room esp32kvse2e --duration 20 --data-channel -v
"""

import argparse
import asyncio
import json
import logging
import time
import uuid

import requests
from aiortc import RTCIceServer, RTCSessionDescription

from kvs_viewer import KvsViewer, ViewerResult, run_viewer

logger = logging.getLogger('apprtc_viewer')

DEFAULT_SERVER = 'https://webrtc.espressif.com'

# How long a joining viewer waits for a device offer before offering itself
OFFER_WAIT_S = 5.0


class AppRtcViewer(KvsViewer):
    """KvsViewer with the room server in place of KVS signaling."""

    def __init__(self, room_id, server_url=DEFAULT_SERVER, client_id=None,
                 duration_s=20.0, connect_timeout_s=60.0, want_audio=True,
                 want_data_channel=False):
        # Deliberately not calling super().__init__: it opens a boto3 session, and
        # the point of this viewer is that no AWS credential is involved.
        self.room_id = room_id
        self.server_url = server_url.rstrip('/')
        self.client_id = client_id or f'pytest-viewer-{uuid.uuid4().hex[:8]}'
        self.duration_s = duration_s
        self.connect_timeout_s = connect_timeout_s
        self.want_audio = want_audio
        self.want_data_channel = want_data_channel
        self.result = ViewerResult(client_id=self.client_id)
        self._pc = None
        self._ws = None
        self._tracks_done = None
        # Same timing fields as KvsViewer.__init__: duration_s is the media window
        # from the moment the peer connection comes up
        self._media_deadline = None
        self._started = time.monotonic()
        self._stop_sampling = asyncio.Event()
        self._early_candidates = []
        self._registered = False
        self._wss_post_url = None

    # ---------------------------------------------------------------- control plane

    def _describe(self):
        """POST /join/<room>, the same call the device makes.

        Returns (None, wss_url, ice_servers) to match KvsViewer.run()'s contract;
        AppRTC has no ARN, and the first element is only passed back to
        _presign_wss, which ignores it here.
        """
        resp = requests.post(f'{self.server_url}/join/{self.room_id}', timeout=15)
        resp.raise_for_status()
        body = resp.json()
        if body.get('result') != 'SUCCESS':
            # FULL means two peers already hold the room: almost always a previous
            # run that never left, and the next line is otherwise a NoneType crash.
            raise RuntimeError(
                f'room {self.room_id!r} refused the join: {body.get("result")!r}. '
                f'If this is FULL, the room still holds an earlier session -- use a '
                f'fresh room id or wait for the server to expire it.')
        params = body['params']

        # The device creates the room, so the viewer must be the second peer. If we
        # are initiator the device never joined, and waiting for its answer would
        # just burn the connect timeout.
        if str(params.get('is_initiator')).lower() == 'true':
            raise RuntimeError(
                f'joined room {self.room_id!r} as initiator, so the device is not in '
                f'it. Check the device booted and joined the same room.')

        # Messages the device posted before we joined (its offer and candidates)
        self._stored = []
        for raw in params.get('messages') or []:
            try:
                self._stored.append(json.loads(raw) if isinstance(raw, str) else raw)
            except json.JSONDecodeError:
                logger.warning('undecodable stored message: %r', str(raw)[:200])

        self.client_id = params['client_id']
        self.result.client_id = self.client_id
        self._wss_post_url = params.get('wss_post_url')

        # The room server sends pc_config as a JSON string, not an object
        pc_config = params.get('pc_config') or {}
        if isinstance(pc_config, str):
            pc_config = json.loads(pc_config) if pc_config else {}
        servers = []
        for entry in pc_config.get('iceServers', []):
            urls = entry.get('urls') or entry.get('url')
            if urls:
                servers.append(RTCIceServer(urls=urls,
                                            username=entry.get('username'),
                                            credential=entry.get('credential')))
        if not servers:
            servers.append(RTCIceServer(urls='stun:stun.l.google.com:19302'))
        logger.info('joined room %s as %s (%d ice servers)',
                    self.room_id, self.client_id, len(servers))
        return None, params['wss_url'], servers

    def _ws_headers(self):
        # The collider refuses a WebSocket without the room server's Origin (HTTP 403)
        return {'Origin': self.server_url}

    async def _negotiate(self, pc):
        """Answer the device's offer when it has one; otherwise offer.

        A device that created the room (esp_camera) posts its offer on registering.
        It reaches the viewer either in the join response (room server) or, stored
        on the collider, over the WebSocket right after we register. Without one
        within OFFER_WAIT_S the viewer offers, as with a device that answers."""
        offer = next((m for m in self._stored if m.get('type') == 'offer'), None)
        early = []
        if offer is None:
            await self._register()
            deadline = asyncio.get_event_loop().time() + OFFER_WAIT_S
            while offer is None:
                left = deadline - asyncio.get_event_loop().time()
                if left <= 0:
                    break
                try:
                    raw = await asyncio.wait_for(self._ws.recv(), timeout=left)
                except asyncio.TimeoutError:
                    break
                payload = self._inner(raw)
                if payload and payload.get('type') == 'offer':
                    offer = payload
                else:
                    early.append(raw)
        if offer is None:
            await super()._negotiate(pc)
            for raw in early:
                await self._handle(raw)
            return

        logger.info('<- SDP offer from the device')
        await pc.setRemoteDescription(RTCSessionDescription(sdp=offer['sdp'], type='offer'))
        for m in self._stored:
            if m.get('type') == 'candidate' and m.get('candidate'):
                await self._add_candidate(m)
        for raw in early:
            await self._handle(raw)
        await pc.setLocalDescription(await pc.createAnswer())
        await self._send('SDP_ANSWER', {'type': 'answer', 'sdp': pc.localDescription.sdp})
        # The exchange is complete once our answer is out
        self.result.answer_received = True

    async def _register(self):
        """The collider drops anything sent before a register."""
        if not self._registered:
            await self._ws.send(json.dumps({
                'cmd': 'register',
                'roomid': self.room_id,
                'clientid': self.client_id,
            }))
            self._registered = True
            logger.debug('-> register %s/%s', self.room_id, self.client_id)

    @staticmethod
    def _inner(raw):
        """The signaling message inside a collider frame, or None."""
        try:
            frame = json.loads(raw)
            inner = frame.get('msg')
            return json.loads(inner) if isinstance(inner, str) else inner
        except (json.JSONDecodeError, AttributeError, TypeError):
            return None

    async def _add_candidate(self, payload):
        await self._pc.addIceCandidate(self._parse_candidate({
            'candidate': payload['candidate'],
            'sdpMid': payload.get('id'),
            'sdpMLineIndex': payload.get('label'),
        }))
        self.result.remote_candidates += 1

    def _presign_wss(self, wss_endpoint, arn):
        """No signing: the collider authorises by room and client id at register."""
        return wss_endpoint

    async def _send(self, action, payload, recipient=None):
        """Map KvsViewer's action names onto collider frames.

        Registration is done here rather than in run() so that run() stays shared:
        the collider drops anything sent before a register.
        """
        await self._register()

        if action == 'SDP_OFFER':
            msg = {'type': 'offer', 'sdp': payload['sdp']}
        elif action == 'SDP_ANSWER':
            msg = {'type': 'answer', 'sdp': payload['sdp']}
        elif action == 'ICE_CANDIDATE':
            msg = {'type': 'candidate',
                   'candidate': payload['candidate'],
                   'id': payload.get('sdpMid'),
                   'label': payload.get('sdpMLineIndex')}
        else:
            logger.debug('-> dropping unsupported action %s', action)
            return
        await self._ws.send(json.dumps({'cmd': 'send', 'msg': json.dumps(msg)}))
        logger.debug('-> %s', action)

    async def _handle(self, raw):
        try:
            frame = json.loads(raw)
        except json.JSONDecodeError:
            logger.warning('undecodable signaling frame: %r', raw[:200])
            return
        if frame.get('error'):
            self.result.errors.append(f'collider: {frame["error"]}')
            logger.error('collider rejected a message: %s', frame['error'])
            return
        inner = frame.get('msg')
        if not inner:
            return
        try:
            payload = json.loads(inner) if isinstance(inner, str) else inner
        except json.JSONDecodeError:
            logger.warning('undecodable inner message: %r', str(inner)[:200])
            return

        mtype = payload.get('type')
        if mtype == 'answer':
            logger.info('<- SDP answer')
            await self._pc.setRemoteDescription(
                RTCSessionDescription(sdp=payload['sdp'], type='answer'))
            self.result.answer_received = True
        elif mtype == 'candidate':
            if not payload.get('candidate'):
                return  # end-of-candidates marker
            await self._add_candidate(payload)
        elif mtype == 'bye':
            self.result.errors.append('device left the room')
        else:
            logger.debug('<- ignoring %s', mtype)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--room', required=True, help='AppRTC room id the device joined')
    ap.add_argument('--server', default=DEFAULT_SERVER, help='AppRTC room server URL')
    ap.add_argument('--duration', type=float, default=20.0)
    ap.add_argument('--connect-timeout', type=float, default=60.0)
    ap.add_argument('--no-audio', action='store_true')
    ap.add_argument('--data-channel', action='store_true')
    ap.add_argument('-v', '--verbose', action='store_true')
    args = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format='%(asctime)s %(levelname)s %(name)s: %(message)s')
    viewer = AppRtcViewer(room_id=args.room, server_url=args.server,
                          duration_s=args.duration,
                          connect_timeout_s=args.connect_timeout,
                          want_audio=not args.no_audio,
                          want_data_channel=args.data_channel)
    result = run_viewer(viewer)
    print(result.summary(), flush=True)
    return 0 if result.answer_received and result.video_packets else 1


if __name__ == '__main__':
    raise SystemExit(main())
