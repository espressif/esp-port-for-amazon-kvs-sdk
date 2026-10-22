"""Host deep-sleep wake with a Wi-Fi init config that differs from the CP's.

The host's esp_wifi_init() on every boot, wake included, reaches the
co-processor. When its config differs from the one the co-processor runs,
esp_hosted re-initialises Wi-Fi: the driver is stopped and the host is sent
STA_STOP and a disconnect. The host's own esp_wifi_start() and
esp_wifi_connect() are what bring the link back. A host that skips them on a
wake leaves the co-processor offline, and every wake after that fails too.

pytest_power_save_emu.py cannot see this: its pair runs the same config on both
sides, so the re-init never happens, and it asserts only that the host wakes.

    pytest pytest_wifi_reinit_emu.py --emu -v
"""

import re
import tempfile
import time
from pathlib import Path

import pytest

import emu_pair
from pytest_power_save_emu import WAKE_PORT, _cycle

pytestmark = pytest.mark.emu

CYCLES = 3
REINIT = 'WiFi init config changed, reinitializing'

# The co-processor gets the low-power buffer counts while the host keeps the
# defaults, so the host's esp_wifi_init() on a wake differs from the CP's own.
_MISMATCH = (
    'CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=4\n'
    'CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=8\n'
    'CONFIG_ESP_WIFI_MGMT_SBUF_NUM=8\n'
)


def _cp_wifi_mismatch_spec():
    overlay = Path(tempfile.gettempdir()) / 'kvs_emu_cp_wifi_mismatch.defaults'
    overlay.write_text(_MISMATCH)
    project, target, _, defaults = emu_pair.CP
    return (project, target, 'build_emu_wifi_mismatch', f'{defaults};{overlay}')
GOT_IP = re.compile(r'sta ip: (\d+\.\d+\.\d+\.\d+)')


@pytest.fixture(scope='module')
def cycles():
    results = []

    def drive(run):
        log = Path(run.log_dir) / 'host.log'
        for c in range(1, CYCLES + 1):
            results.append(_cycle(run, log, c))
        # Let the last wake's reconnect land before teardown.
        time.sleep(30)

    run = emu_pair.run_pair(host_wait_for='PMU deep-sleep entry',
                            emu_seconds=1200, wall_seconds=1200,
                            cp_settle_seconds=150,
                            while_running=drive, wake_port=WAKE_PORT,
                            host_spec=emu_pair.HOST_PS,
                            cp_spec=_cp_wifi_mismatch_spec())
    assert run.while_running_exc is None, \
        f'cycle driver raised: {run.while_running_exc}; logs in {run.log_dir}'
    return run, results


def test_cp_reinitialises_wifi(cycles):
    """The variant exercises the re-init at all; without it the rest is vacuous."""
    run, _ = cycles
    assert REINIT in run.cp_log, f'no Wi-Fi re-init; logs in {run.log_dir}'


def test_cp_gets_its_address_back_after_each_reinit(cycles):
    """After every re-init the co-processor associates and gets an address again."""
    run, _ = cycles
    parts = run.cp_log.split(REINIT)[1:]
    missing = [i + 1 for i, seg in enumerate(parts) if not GOT_IP.search(seg)]
    assert not missing, \
        f're-init(s) {missing} of {len(parts)} never got an address back; logs in {run.log_dir}'


def test_host_wakes_every_cycle(cycles):
    """A co-processor left offline cannot carry the wake packet, so later wakes fail."""
    run, results = cycles
    woke = [w for w, _ in results]
    assert all(w is not None for w in woke), \
        f'wake times {woke}; logs in {run.log_dir}'


def test_host_app_gets_ip_on_every_boot(cycles):
    """Every boot of the host app, wake included, ends its wait with an address."""
    run, _ = cycles
    log = emu_pair.ANSI.sub('', run.host_log)
    failed = log.count('streaming_only: Failed to connect to WiFi')
    assert failed == 0 and 'streaming_only: Connected to WiFi' in log, \
        f'{failed} boot(s) waited out without an address; logs in {run.log_dir}'
