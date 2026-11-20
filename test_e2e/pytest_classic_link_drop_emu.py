# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""Single-chip mode: a runtime Wi-Fi drop, then a viewer session.

webrtc_classic runs Wi-Fi, signaling and media on the P4, through
esp_wifi_remote to a network_adapter co-processor. Once signaling is
connected, `wifi-set` on the P4's console stops Wi-Fi, sets the same
credentials, starts it and connects again. The SoftAP's DHCP server hands back
the same lease. The app must see IP_EVENT_STA_GOT_IP again with that address,
signaling must come back, and a viewer that connects afterwards must get an
answer and video.

This is the single-chip counterpart of pytest_runtime_link_drop_emu.py, which
drops the link from the co-processor in split mode. Here the host itself
stops and starts the station, which goes through esp_hosted's STA_STOP and
STA_START handling on the host rather than network split.

On esp_hosted 3.0.9 the station never came back: after esp_wifi_stop() the
co-processor still counted the station as connecting, so esp_wifi_set_config()
failed with ESP_ERR_WIFI_NOT_STARTED and wifi-set gave up with Wi-Fi stopped.
esp_hosted 3.0.10 fixes it.

Needs AWS credentials and a viewer, like pytest_e2e_emu.py. The build adds
sdkconfig.ci.emu_noaes, because the TLS handshake to KVS hangs under esp-emu
with hardware AES on (see test_e2e/README.md, "Emulator").

    pytest pytest_classic_link_drop_emu.py --emu -v
"""

import re
import time
from pathlib import Path

import pytest

import emu_pair
from kvs_viewer import KvsViewer, run_viewer

pytestmark = pytest.mark.emu

SIGNALING_UP = '[KVS Event] Signaling Connected.'
DROP = 'wifi-set myssid mypassword\n'
DISCONNECTED = 'app_wifi_prov: WiFi disconnected'
GOT_IP = re.compile(r'app_wifi_prov: Got IP: (\d+\.\d+\.\d+\.\d+)')
# Time for the station to re-associate and take its lease.
RECOVER_SECONDS = 120
# Time for signaling to notice the dead socket and connect again.
RESIGNAL_SECONDS = 180
VIEWER_MEDIA_S = 45
# Time for signaling to connect at boot under the emulator.
BOOT_SECONDS = 900


def _after(log: str, marker: str) -> str:
    """The log after the first occurrence of marker, '' when there is none."""
    _, sep, after = log.partition(marker)
    return after if sep else ''


def _after_drop(log: str) -> str:
    """The log after the first disconnect that follows the drop trigger.

    A failed association at boot also logs DISCONNECTED, so the search starts at
    the trigger, not at the start of the log.
    """
    return _after(_after(log, SIGNALING_UP), DISCONNECTED)


@pytest.fixture(scope='module')
def dropped(aws_creds, aws_creds_usable, creds_fragment, kvs_channel, kvs_region, request):
    """One run: signaling up, Wi-Fi dropped from the host, then a viewer session.

    creds_fragment comes first so test_e2e/sdkconfig.creds exists before
    webrtc_classic is built. The viewer runs inside run_pair, which stops both
    emulators before it returns.
    """
    def recover_then_view(run):
        log = Path(run.log_dir) / 'host.log'

        def host():
            return emu_pair.ANSI.sub('', log.read_text(errors='replace'))

        deadline = time.time() + RECOVER_SECONDS
        while time.time() < deadline and not GOT_IP.search(_after_drop(host())):
            time.sleep(2)
        deadline = time.time() + RESIGNAL_SECONDS
        while time.time() < deadline and SIGNALING_UP not in _after_drop(host()):
            time.sleep(2)
        v = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                      duration_s=VIEWER_MEDIA_S, connect_timeout_s=connect_timeout)
        return run_viewer(v)

    connect_timeout = request.config.getoption('--connect-timeout')
    # esp-emu stops both chips at --timeout, so it must outlast the boot wait,
    # both recovery waits and the viewer.
    emu_seconds = (BOOT_SECONDS + RECOVER_SECONDS + RESIGNAL_SECONDS
                   + int(connect_timeout) + VIEWER_MEDIA_S + 60)
    run = emu_pair.run_pair(host_wait_for=SIGNALING_UP,
                            emu_seconds=emu_seconds, wall_seconds=BOOT_SECONDS,
                            host_inject=(SIGNALING_UP, DROP),
                            while_running=recover_then_view,
                            host_spec=_classic_spec(), cp_spec=emu_pair.ADAPTER)
    return run


def _classic_spec():
    """CLASSIC, with hardware AES off for the emulator."""
    spec = emu_pair.CLASSIC
    return (spec[0], spec[1], spec[2], spec[3] + ';sdkconfig.ci.emu_noaes')


def _host(run) -> str:
    return emu_pair.ANSI.sub('', run.host_log)


def test_no_emulator_faults(dropped):
    assert not dropped.faults(), f'{dropped.faults()[:3]}; logs in {dropped.log_dir}'


def test_signaling_connected_before_the_drop(dropped):
    assert SIGNALING_UP in _host(dropped), \
        f'signaling never connected; logs in {dropped.log_dir}'


def test_host_sees_the_drop(dropped):
    assert DISCONNECTED in _after(_host(dropped), SIGNALING_UP), \
        f'no disconnect after signaling came up; logs in {dropped.log_dir}'


def test_host_gets_the_same_ip_after_the_drop(dropped):
    log = _host(dropped)
    boot = GOT_IP.findall(log.partition(SIGNALING_UP)[0])
    again = GOT_IP.findall(_after_drop(log))
    assert again, f'no GOT_IP after the drop; logs in {dropped.log_dir}'
    assert boot and again[0] == boot[-1], f'boot {boot}, after the drop {again}'


def test_signaling_comes_back(dropped):
    assert SIGNALING_UP in _after_drop(_host(dropped)), \
        f'signaling did not connect again after the drop; logs in {dropped.log_dir}'


def test_viewer_gets_the_answer_after_the_drop(dropped):
    assert dropped.while_running_exc is None, \
        f'viewer raised: {dropped.while_running_exc}; logs in {dropped.log_dir}'
    result = dropped.while_running_result
    assert result and result.answer_received, \
        f'no answer: {result.summary() if result else "no viewer"}; logs in {dropped.log_dir}'


def test_viewer_decodes_video_after_the_drop(dropped):
    result = dropped.while_running_result
    if not result or not result.video_packets:
        pytest.skip(f'no media through the NAT: {result.summary() if result else "no viewer"}')
    print(f'EMU CLASSIC AFTER DROP: {result.summary()}')
    assert result.frames_video > 0, \
        f'video packets arrived but none decoded: {result.summary()}; logs in {dropped.log_dir}'
