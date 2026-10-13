# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""End-to-end streaming test for the webrtc_classic example.

Boots the device, watches the signaling state machine walk to CONNECTED, connects a
real aiortc viewer, and asserts media flowed over a peer-to-peer candidate pair.

    pytest pytest_webrtc_classic.py --e2e --target esp32p4 -p /dev/ttyUSB0 -s
"""

import logging
import pathlib
import re

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

from conftest import REPO_ROOT
from kvs_viewer import KvsViewer, run_viewer

logger = logging.getLogger(__name__)

APP_PATH = str(REPO_ROOT / 'examples' / 'webrtc_classic')

# kvs_signaling.c:530 logs the upstream SIGNALING_CLIENT_STATE enum (Include.h:1092),
# which is the actual signaling state machine walk and is visible at INFO. Asserting
# these in sequence is the state-transition check.
#
# CREATE (4) is skipped for a channel that already exists, so it is not asserted.
SIGNALING_STATE_WALK = (
    (1, 'NEW'),
    (2, 'GET_CREDENTIALS'),
    (3, 'DESCRIBE'),
    (5, 'GET_ENDPOINT'),
    (6, 'GET_ICE_CONFIG'),
    (7, 'READY'),
    (8, 'CONNECTING'),
    (9, 'CONNECTED'),
)

# Printed as a batch *after* the walk completes, not as per-phase progress.
SIGNALING_TIMING_SUMMARY = (
    b'[Signaling Get token]',
    b'[Signaling Describe]',
    b'[Signaling Get endpoint]',
    b'[Signaling Get ICE config]',
    b'[Signaling Connect]',
)
# RTC_PEER_CONNECTION_STATE_CONNECTED as logged by kvs_webrtc.c:1155
PEER_STATE_CONNECTED = 3

BOOT_TIMEOUT = 60
WIFI_TIMEOUT = 120
# Candidate types that mean the media path is direct. 'relay' is the TURN fallback.
P2P_CANDIDATE_TYPES = ('host', 'srflx', 'prflx')


def _expect(dut, pattern, timeout, label):
    """dut.expect, but a timeout names the phase that stalled instead of dumping a
    pexpect traceback."""
    try:
        return dut.expect(pattern, timeout=timeout)
    except Exception as exc:
        raise AssertionError(
            f'{label}: never saw {pattern!r} within {timeout}s ({type(exc).__name__})') from exc


def _seen(dut, pattern, timeout):
    """True if the pattern shows up before the timeout. Absence is a timeout, so this
    is the only correct way to assert a line did *not* appear."""
    try:
        dut.expect(pattern, timeout=timeout)
        return True
    except Exception:
        return False


def _log_blob(dut):
    """The whole session log so far, ANSI colour stripped."""
    blob = (pathlib.Path(dut.logdir) / 'dut.log').read_bytes()
    return re.sub(rb'\x1b\[[0-9;]*m', b'', blob)


def _in_log(dut, pattern):
    """Search the session log without consuming the expect buffer -- for lines that
    may already have scrolled past, or whose order relative to others is not stable."""
    return re.search(pattern, _log_blob(dut))


def assert_ordered_in_log(dut, patterns, label):
    """Assert `patterns` appear, in this order, anywhere in the session log.

    dut.expect() consumes forward, which makes it the wrong tool for a sequence the
    device may have produced *before* the test got to it. That happens here for real:
    a board that boots already-provisioned completes the whole signaling walk in the
    first seconds, so a forward expect() for state 1 times out even though the walk
    happened perfectly. Reading the log once and checking relative order is immune to
    that, and still fails if a state is missing or out of sequence.

    Returns the list of match objects, so callers can pull captured groups out.
    """
    blob = _log_blob(dut)

    matches, pos = [], 0
    for pattern in patterns:
        rx = pattern if hasattr(pattern, 'search') else re.compile(
            re.escape(pattern.encode() if isinstance(pattern, str) else pattern))
        m = rx.search(blob, pos)
        if m is None:
            later = rx.search(blob)
            hint = (' (it appears earlier in the log, so it arrived out of order)'
                    if later else ' (it never appears at all)')
            raise AssertionError(f'{label}: {rx.pattern!r} missing{hint}')
        matches.append(m)
        pos = m.end()
    return matches


def assert_signaling_walk(dut, expected, label, slave=False):
    """Assert the signaling machine reached CONNECTED having visited every state.

    Reads every "KVS signaling state changed to N" the session logged, then checks the
    attempt that actually connected: the window from the last NEW before the final
    CONNECTED, through that CONNECTED. Inside it, each expected state must first appear
    after the previous one did.

    The two obvious rules are both wrong against real devices:

    - *contiguous* run of the exact sequence -- too strict. A healthy device retries
      mid-walk; an observed run was 1,2,3,5,6,7,8,5,6,3,5,6,7,8,9, looping back to
      GET_ENDPOINT and DESCRIBE before connecting.
    - *in order anywhere in the log* -- too weak. A device that reconnects walks the
      states repeatedly, so an incomplete early attempt can be stitched onto a later
      one and pass when no single attempt ever completed.

    Scoping to the connecting attempt accepts retries while still failing a skipped
    state, an out-of-order progression, or never connecting at all.

    `slave=True` restricts the search to lines the P4 forwarded for the C6.
    """
    blob = _log_blob(dut)
    line_rx = re.compile(
        (rb'\[SLAVE\][^\n]*?' if slave else rb'')
        + rb'KVS signaling state changed to (\d+)')
    observed = [int(m.group(1)) for m in line_rx.finditer(blob)]
    want = [v for v, _ in expected]
    names = dict(expected)
    first, last = want[0], want[-1]

    def fail(why):
        raise AssertionError(
            f'{label}: {why}. Expected to visit '
            f'{[f"{v}:{names[v]}" for v in want]}; states actually logged, in order: '
            f'{observed or "none at all"}')

    if last not in observed:
        fail(f'never reached {last}:{names[last]}')
    end = len(observed) - 1 - observed[::-1].index(last)
    head = observed[:end]
    begin = len(head) - 1 - head[::-1].index(first) if first in head else 0

    attempt = observed[begin:end + 1]
    pos = -1
    for value in want:
        if value not in attempt[pos + 1:]:
            fail(f'{value}:{names[value]} missing from (or out of order in) the '
                 f'attempt that connected: {attempt}')
        pos = pos + 1 + attempt[pos + 1:].index(value)

    logger.info('%s: reached %s via %s', label, f'{last}:{names[last]}', attempt)
    return observed


@pytest.mark.e2e
@pytest.mark.parametrize('app_path', [APP_PATH], indirect=True)
# esp32s3 is a supported target for this example but is not covered here -- it
# has different media capabilities and has not been validated against this
# oracle. Add it once someone runs it on that board.
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_e2e_streaming(dut: Dut, request, aws_creds, wifi_creds, creds_fragment,
                       kvs_channel, kvs_region, channel_matches_firmware) -> None:
    ssid, password = wifi_creds
    allow_relay = request.config.getoption('--allow-relay')
    media_timeout = request.config.getoption('--media-timeout')
    connect_timeout = request.config.getoption('--connect-timeout')
    logger.info('device credential fragment: %s', creds_fragment)

    # -- Phase 1: boot and join Wi-Fi ------------------------------------------
    # wifi-set means the test does not depend on the SSID the firmware was built with,
    # nor on BLE provisioning completing. It is harmless if the board was already
    # provisioned -- it just restarts the station against the same AP.
    _expect(dut, 'ESP32 WebRTC Example', BOOT_TIMEOUT, 'boot')
    dut.write(f'wifi-set {ssid} {password}')
    # app_wifi_prov.c logs 'Got IP: <addr>'; esp_netif also emits 'sta ip: <addr>'.
    ip = _expect(dut, re.compile(rb'(?:Got IP|sta ip): (\d+\.\d+\.\d+\.\d+)'),
                 WIFI_TIMEOUT, 'wifi connect')
    logger.info('device IP: %s', ip.group(1).decode())

    # -- Phase 2: signaling state walk ----------------------------------------
    # Wait for the end of the walk with a forward expect (so the test blocks until
    # signaling is actually up), then check the whole ordered sequence in the log.
    _expect(dut, re.compile(rb'\[KVS Event\] Signaling Connected\.'),
            connect_timeout, 'signaling connected event')

    assert_signaling_walk(dut, SIGNALING_STATE_WALK, 'signaling state walk')
    # The timing summary follows the walk; its presence confirms every control-plane
    # call reported a duration rather than being skipped.
    assert_ordered_in_log(dut, list(SIGNALING_TIMING_SUMMARY), 'signaling timings')

    # -- Phase 3: viewer -------------------------------------------------------
    viewer = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                       duration_s=media_timeout, connect_timeout_s=connect_timeout)
    result = run_viewer(viewer)
    logger.info('viewer result: %s', result.summary())

    assert result.answer_received, f'device never answered the offer: {result.summary()}'

    # Packets, not decoded frames: aiortc's H.264 decoder rejects some valid streams,
    # so asserting on frames_video would make this flaky. Frame counts are logged.
    assert result.video_packets > 0, f'no video RTP reached the viewer: {result.summary()}'
    # Boards without an audio codec stream video only and say so; do not fail them.
    if _in_log(dut, rb'Audio capture not available'):
        logger.warning('board has no audio capture; audio RTP not asserted')
    else:
        assert result.audio_packets > 0, \
            f'no audio RTP reached the viewer: {result.summary()}'
    # Loss is reported, not asserted, by default: set --max-video-loss once a
    # baseline measured with per-SSRC counting exists for the board.
    total = result.video_packets + result.video_packets_lost
    loss = result.video_packets_lost / total if total else 0.0
    logger.info('inbound video: %d packets, %d lost (%.0f%% loss), %d frames decoded, '
                'first frame at %ss',
                result.video_packets, result.video_packets_lost, loss * 100,
                result.frames_video, result.first_video_latency_s)
    max_loss = request.config.getoption('--max-video-loss')
    assert loss <= max_loss, (
        f'video packet loss {loss:.0%} exceeds --max-video-loss={max_loss:.0%}: '
        f'{result.summary()}')

    # -- Phase 4: device-side session assertions -------------------------------
    # dut.expect reads a persistent buffer, so these lines were emitted while the
    # viewer above was running and are still waiting to be matched.
    #
    # The order below is taken from real sessions, not from reading the code, and is
    # not the intuitive one: "Streaming Started" fires right after the answer, well
    # before the connection is actually up, while "Peer Connected" comes last.
    #   Sent Answer -> Streaming Started -> Selected pair -> state=3
    #     -> Media started -> Peer Connected
    cid = result.client_id
    _expect(dut, re.compile(rb'\[KVS Event\] Sent Answer\.'), 15, 'device sent answer')
    _expect(dut, f'Streaming Started for Peer: {cid}', 15, 'device streaming started')

    # The P2P oracle. This line is DLOGP, which kvs_utils/logger.c:94 maps to
    # ESP_LOG_ERROR, so it survives the KVS logger's default WARN gate unconfigured.
    pair = _expect(dut, re.compile(
        rb'Selected pair \S+, local candidate type: (\w+)\. '
        rb'remote candidate type: (\w+)\.'), 15, 'selected candidate pair')
    local_type = pair.group(1).decode()
    remote_type = pair.group(2).decode()
    logger.info('device selected pair: local=%s remote=%s', local_type, remote_type)

    if not allow_relay:
        # Relay on either end means TURN carries the media: a local srflx paired with
        # the viewer's relay candidate is still a relayed session.
        assert local_type in P2P_CANDIDATE_TYPES and remote_type in P2P_CANDIDATE_TYPES, (
            f'device selected a {local_type}/{remote_type} pair -- media is being '
            f'relayed through TURN, not peer-to-peer. Pass --allow-relay if this '
            f'network genuinely requires it.')

    _expect(dut, f'State change: peer={cid}, state={PEER_STATE_CONNECTED}',
            15, 'device peer state CONNECTED')
    _expect(dut, f'Media started for peer={cid}', 15, 'device media started')
    _expect(dut, f'Peer Connected: {cid}', 15, 'device peer connected')

    # Steady-state send rate. Needs CONFIG_KVS_WEBRTC_MEDIA_DIAG_LOG=y (sdkconfig.ci.e2e).
    try:
        diag = dut.expect(re.compile(rb'DIAG frame_cb: send_v_fps=(\d+)'), timeout=5)
        fps = int(diag.group(1))
        assert fps > 0, 'device reported send_v_fps=0 while the viewer was seeing frames'
        logger.info('device send_v_fps=%d', fps)
    except AssertionError:
        raise
    except Exception:
        logger.warning('no DIAG frame_cb line -- is CONFIG_KVS_WEBRTC_MEDIA_DIAG_LOG=y?')

    # -- Phase 5: nothing went irrecoverably wrong ------------------------------
    # A KVS error event is reported, not asserted. Signaling recovers from transient
    # TLS/socket failures by reconnecting, and one during teardown says nothing about
    # the session that just succeeded -- every property we care about (P2P pair, RTP
    # in both directions, peer connected) has already been asserted above. Failing on
    # any error event made this flaky on a lossy AP.
    if _seen(dut, re.compile(rb'\[KVS Event\] Error Event'), 2):
        logger.warning('device raised a KVS error event (signaling usually recovers '
                       'by reconnecting); check the log if other assertions failed')

    # The watchdog is different: it means the peer was written off as dead, which
    # contradicts the session having worked.
    assert not _seen(dut, 'Liveness watchdog', 2), \
        'device liveness watchdog tripped -- the peer was declared dead'


@pytest.mark.e2e
@pytest.mark.parametrize('app_path', [APP_PATH], indirect=True)
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_e2e_data_channel(dut: Dut, request, aws_creds, wifi_creds, creds_fragment,
                          kvs_channel, kvs_region, channel_matches_firmware) -> None:
    """Data channel comes up over DTLS/SCTP.

    Separate from the media test because it exercises a different transport: SCTP can
    break while RTP keeps working (and vice versa). Needs CONFIG_ENABLE_DATA_CHANNEL=y,
    which webrtc_classic has by default.

    webrtc_classic registers no callbacks of its own, but the SDK's own data channel
    handler echoes what it receives, so both the open and the round trip are checked.
    """
    ssid, password = wifi_creds
    connect_timeout = request.config.getoption('--connect-timeout')

    _expect(dut, 'ESP32 WebRTC Example', BOOT_TIMEOUT, 'boot')
    dut.write(f'wifi-set {ssid} {password}')
    _expect(dut, re.compile(rb'\[KVS Event\] Signaling Connected\.'),
            connect_timeout, 'signaling connected')

    viewer = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                       duration_s=10.0, connect_timeout_s=connect_timeout,
                       want_data_channel=True)
    result = run_viewer(viewer)
    logger.info('viewer result: %s', result.summary())

    assert result.answer_received, f'device never answered: {result.summary()}'
    assert result.dc_opened, (
        f'data channel never opened -- SCTP over DTLS did not come up: '
        f'{result.summary()}')
    assert result.dc_echo == 'ping-from-pytest', (
        f'data channel opened but the round trip failed: expected the device to echo '
        f'our message, got {result.dc_echo!r}. {result.summary()}')
