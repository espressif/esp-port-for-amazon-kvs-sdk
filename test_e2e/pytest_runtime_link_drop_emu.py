# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""A runtime Wi-Fi drop on the co-processor, after the pair is up.

The co-processor owns the link. When it drops and re-associates, the DHCP
server hands it the same lease, and network split relays that status to the
host. The host app cleared its IP state on the disconnect and waits for
IP_EVENT_STA_GOT_IP, as on plain IDF, so the status has to come back as
GOT_IP. esp_hosted 3.0.7 to 3.0.9 dropped it: the host's static netif kept the
old address across the drop, so the same lease looked like a replay and was
skipped. Nothing then set the app's GOT_IP bit again, and a hosted_recovery
restore that waited for it timed out. esp_hosted 3.0.10 fixes it.

pytest_wifi_reinit_emu.py cannot show this: its disconnect happens while the
host app is still in its boot-time IP wait, which recovers on its own.

The drop is driven from the co-processor's console: `wifi-set` stops Wi-Fi,
sets the same credentials, starts it and connects again.

    pytest pytest_runtime_link_drop_emu.py --emu -v
"""

import re
import time
from pathlib import Path

import pytest

import emu_pair

pytestmark = pytest.mark.emu

# The host is up and has announced itself; anything after this is runtime.
CP_UP = 'Received READY signal from P4'
# The host's side of the same point: the drop comes after it.
HOST_UP = 'READY signal sent successfully'
DROP = 'wifi-set myssid mypassword\n'
DISCONNECTED = 'streaming_only: WiFi disconnected'
GOT_IP = re.compile(r'streaming_only: got ip:(\d+\.\d+\.\d+\.\d+)')
CP_IP = re.compile(r'sta ip: (\d+\.\d+\.\d+\.\d+)')
# Time for the co-processor to re-associate, take its lease and relay it.
RECOVER_SECONDS = 120
# Time for the pair to come up under the emulator.
BOOT_SECONDS = 600
# Longest wait for CP_UP after the host marker (run_pair's cp_settle_seconds).
CP_SETTLE_SECONDS = 120


def _after(log: str, marker: str) -> str:
    """The log after the first occurrence of marker, '' when there is none."""
    _, sep, after = log.partition(marker)
    return after if sep else ''


def _host_after_drop(log: str) -> str:
    """The host log after the first disconnect that follows HOST_UP.

    A failed association at boot also logs DISCONNECTED, so the search starts at
    HOST_UP, not at the start of the log.
    """
    return _after(_after(log, HOST_UP), DISCONNECTED)


@pytest.fixture(scope='module')
def dropped():
    def wait_for_recovery(run):
        log = Path(run.log_dir) / 'host.log'
        deadline = time.time() + RECOVER_SECONDS
        while time.time() < deadline:
            after = _host_after_drop(emu_pair.ANSI.sub('', log.read_text(errors='replace')))
            if GOT_IP.search(after):
                break
            time.sleep(2)

    # esp-emu stops both chips at --timeout, so it must outlast the boot wait,
    # the CP settle wait and the recovery wait.
    run = emu_pair.run_pair(host_wait_for=HOST_UP,
                            emu_seconds=BOOT_SECONDS + CP_SETTLE_SECONDS + RECOVER_SECONDS + 60,
                            wall_seconds=BOOT_SECONDS,
                            cp_inject=(CP_UP, DROP), cp_wait_for=CP_UP,
                            cp_settle_seconds=CP_SETTLE_SECONDS,
                            while_running=wait_for_recovery)
    assert run.while_running_exc is None, \
        f'recovery wait raised: {run.while_running_exc}; logs in {run.log_dir}'
    return run


def test_no_emulator_faults(dropped):
    assert not dropped.faults(), f'{dropped.faults()[:3]}; logs in {dropped.log_dir}'


def test_cp_drops_and_gets_the_same_lease(dropped):
    """The drop happened at runtime and the co-processor got its lease back."""
    run = dropped
    assert CP_UP in run.cp_log, f'host never sent READY; logs in {run.log_dir}'
    after = run.cp_log.split(CP_UP, 1)[1]
    before_ips = CP_IP.findall(run.cp_log.split(CP_UP, 1)[0])
    after_ips = CP_IP.findall(after)
    assert before_ips and after_ips, \
        f'CP addresses before {before_ips}, after {after_ips}; logs in {run.log_dir}'
    assert after_ips[0] == before_ips[-1], \
        f'lease changed {before_ips[-1]} -> {after_ips[0]}; the case needs the same lease'


def test_host_sees_the_drop(dropped):
    log = emu_pair.ANSI.sub('', dropped.host_log)
    assert DISCONNECTED in _after(log, HOST_UP), \
        f'no host disconnect after {HOST_UP!r}; logs in {dropped.log_dir}'


def test_host_gets_ip_after_the_drop(dropped):
    """The app's own GOT_IP comes back, with the same address, after the drop."""
    log = emu_pair.ANSI.sub('', dropped.host_log)
    boot = GOT_IP.findall(log.partition(HOST_UP)[0])
    again = GOT_IP.findall(_host_after_drop(log))
    assert again, f'no GOT_IP after the runtime disconnect; logs in {dropped.log_dir}'
    assert boot and again[0] == boot[-1], f'boot {boot}, after the drop {again}'
