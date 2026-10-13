# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""End-to-end streaming test for split mode (ESP32-P4 + ESP32-C6).

The C6 runs `signaling_only` and holds the AWS KVS WebSocket; the P4 runs
`streaming_only` and does RTP directly with the viewer. Media never round-trips
through the C6.

This is a **single-DUT** test even though two chips are involved. On a one-USB-port
kit like P4-EYE there is no separate C6 console, so:

  - the C6's log arrives on the P4's console, re-printed with a `[SLAVE]` prefix by
    the slave_flasher monitor task (`slave_flasher.c:116`, continuous task started
    at `:414`, gated on CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR)
  - the C6's console is written through the P4's `write_slave <data>` command
    (`slave_flasher.c:321`)

Boards with a separate C6 UART (p4_function_ev_board_*) could instead run this as a
two-DUT test; the single-port path is the more constrained one, so it is what this
covers.

    pytest pytest_split_mode.py --e2e --target esp32p4 -p /dev/cu.usbmodem12101 -s
"""

import logging
import re
import time

import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize

from conftest import REPO_ROOT, _built_config
from kvs_viewer import KvsViewer, run_viewer
from pytest_webrtc_classic import (BOOT_TIMEOUT, P2P_CANDIDATE_TYPES,
                                   SIGNALING_STATE_WALK, WIFI_TIMEOUT, _expect, _in_log,
                                   _seen, assert_signaling_walk)

logger = logging.getLogger(__name__)

APP_PATH = str(REPO_ROOT / 'examples' / 'streaming_only')

# RTC_PEER_CONNECTION_STATE_CONNECTED, as logged by kvs_webrtc.c
PEER_STATE_CONNECTED = 3

# The C6 has to flash (first boot only) and then boot before its log appears.
SLAVE_BOOT_TIMEOUT = 180


def _slave(pattern):
    """Match a C6 line, re-printed by the P4 on the C6's behalf.

    Both chips' output lands on one console, tagged by the slave_flasher monitor:
    the C6's lines carry "[SLAVE]" and the P4's own carry "[HOST]", each wrapped in
    ANSI colour. Anchoring on the tag is what stops a C6 assertion being satisfied
    by a P4 line that logs the same text, and vice versa.
    """
    if isinstance(pattern, str):
        pattern = re.escape(pattern.encode())
    return re.compile(rb'\[SLAVE\].*?' + pattern)


# Whether the P4 tags its own lines with [HOST]. Only the custom_vprintf that
# streaming_only installs under CONFIG_SLAVE_FLASHER_ENABLE adds the tag, because
# only then does the C6's log share the P4's console. Set per test from the build.
_HOST_TAGGED = True


@pytest.fixture(autouse=True)
def _host_tag(request):
    global _HOST_TAGGED
    _, cfg = _built_config(request)
    _HOST_TAGGED = bool(cfg.get('SLAVE_FLASHER_ENABLE', False))
    yield


def _host(pattern):
    """Match a line the P4 logged itself.

    With the slave flasher the C6's log is re-printed on the P4's console, and
    anchoring on the [HOST] tag is what stops a P4 assertion being satisfied by a
    C6 line that logs the same text. Without it (the v12/v16 EV-board overlays)
    the P4 prints no tag and its console carries only its own log.
    """
    if isinstance(pattern, str):
        pattern = re.escape(pattern.encode())
    if not _HOST_TAGGED:
        return re.compile(pattern)
    return re.compile(rb'\[HOST\].*?' + pattern)


def _write_slave(dut, command, attempts=4):
    """Send a line to the C6's console through the P4, and prove it got there.

    The P4's REPL runs on a 256-byte buffer with no flow control, so a command
    issued while the boot log is still streaming loses leading characters --
    observed as `write_slave` arriving as `rite_slave` followed by "Unrecognized
    command", after which the C6 never sees wifi-set and the test fails much later
    with a misleading "C6 wifi connect" timeout. Nudge the REPL onto a fresh
    prompt, then retry until the P4 confirms it forwarded the bytes.
    """
    for attempt in range(1, attempts + 1):
        dut.write('')                      # settle the REPL on a clean prompt
        time.sleep(1.0)
        dut.write(f'write_slave "{command}"')
        try:
            dut.expect(re.compile(rb'Sent \d+ bytes to slave'), timeout=10)
            return
        except Exception:
            logger.warning('write_slave not acknowledged (attempt %d/%d); retrying',
                           attempt, attempts)
    raise AssertionError(
        f'the P4 never acknowledged forwarding {command!r} to the C6 after '
        f'{attempts} attempts -- its console is dropping characters')


@pytest.mark.e2e
@pytest.mark.parametrize('app_path', [APP_PATH], indirect=True)
@idf_parametrize('target', ['esp32p4'], indirect=['target'])
def test_split_mode_streaming(dut: Dut, request, aws_creds, wifi_creds, creds_fragment,
                              kvs_channel, kvs_region, channel_matches_firmware) -> None:
    ssid, password = wifi_creds
    allow_relay = request.config.getoption('--allow-relay')
    media_timeout = request.config.getoption('--media-timeout')
    connect_timeout = request.config.getoption('--connect-timeout')
    logger.info('device credential fragment (built into the C6): %s', creds_fragment)

    # -- Phase 1: P4 boots and the C6 comes up behind it -----------------------
    _expect(dut, _host('ESP32 WebRTC Streaming Example'), BOOT_TIMEOUT, 'P4 boot')
    # Any [SLAVE] line proves both that the C6 is running and that the forwarding
    # path works. Without it the C6-side assertions would all fail identically,
    # which tells you nothing about which half broke.
    _expect(dut, re.compile(rb'\[SLAVE\]'), SLAVE_BOOT_TIMEOUT,
            'C6 log forwarding (is CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR=y?)')
    # -- Phase 2: the C6 reaches AWS ------------------------------------------
    # Wait on the *last* C6 milestone and check the earlier ones by scanning the log
    # afterwards. Consuming expect() calls in a fixed order cannot work here: the P4's
    # bridge lines land ~13 s into boot while the C6's Wi-Fi timing depends on whether
    # it booted already-provisioned, so either ordering skips past the other chip's
    # line and fails on something that did happen.
    #
    # Do not provision unconditionally. wifi-set restarts the station, and on a C6 that
    # was already connected that tears down a working signaling session -- the observed
    # recovery was 66 s of TLS retries, longer than the budget this test allows. So
    # provision only when the C6 does not get there by itself.
    if not _seen(dut, _slave('Signaling connected successfully'), SLAVE_BOOT_TIMEOUT):
        logger.info('C6 did not reach signaling on its own; provisioning over write_slave')
        _write_slave(dut, f'wifi-set {ssid} {password}')
        _expect(dut, _slave('Signaling connected successfully'), connect_timeout * 2,
                'C6 signaling connected after wifi-set')

    # -- Phase 3: everything that had to happen to get here --------------------
    ip = _in_log(dut, _slave(rb'(?:Got IP|sta ip): (\d+\.\d+\.\d+\.\d+)'))
    assert ip, 'the C6 reached signaling but never logged an IP -- log forwarding broke?'
    logger.info('C6 IP: %s', ip.group(1).decode())
    assert _in_log(dut, _host('Bridge signaling client connected')), \
        'the P4 never joined the bridge, so an offer could not reach it'
    assert _in_log(dut, _host('READY signal sent successfully')), \
        'the P4 never told the C6 it can accept offers'

    assert_signaling_walk(dut, SIGNALING_STATE_WALK, 'C6 signaling state walk',
                          slave=True)

    # -- Phase 4: viewer -------------------------------------------------------
    viewer = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                       duration_s=media_timeout, connect_timeout_s=connect_timeout)
    result = run_viewer(viewer)
    logger.info('viewer result: %s', result.summary())

    if not result.answer_received:
        # Distinguish "the P4 ignored a delivered offer" from "the offer never got
        # there". The C6's KVS WebSocket does drop (esp_transport_read ... errno=113),
        # and when it does the offer is lost in transit -- a different fault from the
        # peer connection failing, and misreporting it sends you debugging the P4.
        dropped = _in_log(dut, _slave(rb'esp_transport_read\(\) failed'))
        hint = (' -- the C6 lost its KVS WebSocket, so the offer never reached the '
                'device; this is a signaling drop, not a peer connection failure'
                if dropped else '')
        raise AssertionError(f'no answer came back{hint}: {result.summary()}')
    assert result.video_packets > 0, f'no video RTP reached the viewer: {result.summary()}'
    # Not every P4 board has an audio codec -- the P4+C5 core board has none, and the
    # firmware says so and streams video only. Asserting audio there fails a device
    # that is behaving correctly, so take the device's own word for what it can do.
    if _in_log(dut, _host('Audio capture not available')):
        logger.warning('board has no audio capture; audio RTP not asserted')
    else:
        assert result.audio_packets > 0, \
            f'no audio RTP reached the viewer: {result.summary()}'

    # -- Phase 5: the offer crossed the bridge and the P4 served it ------------
    # This is the split-mode-specific part: the offer arrived on the C6, was queued
    # and flushed over the bridge, and the P4 answered it.
    #
    # Order matters and is not the obvious one -- taken from a real session:
    #   Sent Answer -> Selected pair -> state=3 -> Media started -> Peer Connected
    # Of the event cases streaming_only has, only "Sent Answer" and "Peer Connected"
    # are ever raised; RECEIVED_OFFER and STREAMING_STARTED are dead on this path,
    # so media start comes from kvs_webrtc's own wording.
    cid = result.client_id
    _expect(dut, _host(rb'\[KVS Event\] Sent Answer\.'), 15,
            'P4 answered over the bridge')

    # Media is P4 <-> viewer directly; the C6 must not be in the path.
    pair = _expect(dut, _host(
        rb'Selected pair \S+, local candidate type: (\w+)\. '
        rb'remote candidate type: (\w+)\.'), 15, 'P4 selected candidate pair')
    local_type, remote_type = pair.group(1).decode(), pair.group(2).decode()
    logger.info('P4 selected pair: local=%s remote=%s', local_type, remote_type)
    if not allow_relay:
        assert local_type in P2P_CANDIDATE_TYPES and remote_type in P2P_CANDIDATE_TYPES, (
            f'P4 selected a {local_type}/{remote_type} pair -- media is relayed through '
            f'TURN rather than going straight to the viewer. Pass --allow-relay if '
            f'this network requires it.')

    _expect(dut, _host(f'State change: peer={cid}, state={PEER_STATE_CONNECTED}'),
            15, 'P4 peer state CONNECTED')
    _expect(dut, _host(f'Media started for peer={cid}'), 15, 'P4 media started')
    _expect(dut, _host(f'Peer Connected: {cid}'), 15, 'P4 peer connected')

    # Steady-state send rate (CONFIG_KVS_WEBRTC_MEDIA_DIAG_LOG=y in sdkconfig.ci.e2e):
    # the P4 is pushing frames, not just holding a session.
    diag = _expect(dut, _host(rb'DIAG frame_cb: send_v_fps=(\d+)'), 10, 'P4 send rate')
    fps = int(diag.group(1))
    assert fps > 0, 'P4 reported send_v_fps=0 while the viewer was receiving video'
    logger.info('P4 send_v_fps=%d', fps)

    total = result.video_packets + result.video_packets_lost
    loss = result.video_packets_lost / total if total else 0.0
    logger.info('inbound video: %d packets, %d lost (%.0f%% loss), %d frames decoded',
                result.video_packets, result.video_packets_lost, loss * 100,
                result.frames_video)
    max_loss = request.config.getoption('--max-video-loss')
    assert loss <= max_loss, (
        f'video packet loss {loss:.0%} exceeds --max-video-loss={max_loss:.0%}: '
        f'{result.summary()}')

    # -- Phase 6: nothing was silently dropped ---------------------------------
    # The adapter frees flushed messages with no retry, so a full queue loses a
    # viewer's offer outright. It must never happen.
    assert not _seen(dut, 'Queue full, dropping newest message', 2), \
        'the bridge adapter dropped a signaling message -- an offer can be lost this way'
    # Reported, not asserted -- see the same check in pytest_webrtc_classic.py.
    if _seen(dut, re.compile(rb'\[KVS Event\] Error Event'), 2):
        logger.warning('a KVS error event was raised (signaling usually recovers); '
                       'check the log if other assertions failed')
    assert not _seen(dut, 'Liveness watchdog', 2), \
        'the liveness watchdog tripped -- the peer was declared dead'

    # -- Phase 7: the P4 goes back to idling (reported, not asserted) ----------
    # Split mode's point is that the P4 idles until a viewer turns up, and the timer
    # is armed from the PEER_DISCONNECTED handler once the active-peer count hits
    # zero (power_save_handler.c:96). It is only *reported* here: aiortc's close()
    # sends nothing over signaling, so the P4 keeps streaming until its own ICE
    # timeout fires, which was still pending well over a minute later in testing.
    # Asserting it would add minutes of dead wait for a secondary property.
    if _seen(dut, _host(f'Peer disconnected: {cid}'), 15):
        if _seen(dut, _host('All peer connections closed, starting host power save timer'), 10):
            logger.info('P4 re-armed host power save after the session')
        else:
            logger.warning('P4 saw the peer leave but did not re-arm host power save')
    else:
        logger.info('P4 had not yet noticed the viewer leaving; power-save re-arm '
                    'not checked (it waits on the P4 ICE timeout)')
