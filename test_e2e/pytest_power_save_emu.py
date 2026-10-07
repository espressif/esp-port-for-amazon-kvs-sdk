"""Host deep sleep and wake, over multiple cycles, under ESP-EMU.

The example's automatic deep-sleep timer is off in every other emu build, so
nothing else here exercises it. sdkconfig.ci.emu_ps turns it on.

What makes this testable without a bench: esp-emu models the P4 PMU deep-sleep
entry, and the hosted bridge carries the co-processor's wake line as OP_WAKE, so
a packet fired at the forwarded host-reserved port wakes a sleeping host exactly
as traffic from the network would. Without --hosted-wake-gpio there is no wake
line at all and a slept host never returns, which is why run_pair takes a
wake_port rather than wiring it unconditionally.

What this does not prove: parked current, and the co-processor's own light
sleep residency. Both need the bench.

    pytest pytest_power_save_emu.py --emu -v
"""

import socket
import struct
import time
from pathlib import Path

import pytest

import emu_pair

pytestmark = pytest.mark.emu

WAKE_PORT = 45080
CYCLES = 3


def _poke(payload=b'wakeup-host'):
    """Fire and forget at the host-reserved port. Nothing listens; the point is
    that the frame reaches the co-processor and is routed at the host."""
    try:
        s = socket.socket()
        s.settimeout(2)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
        s.connect(('127.0.0.1', WAKE_PORT))
        s.sendall(payload)
        s.close()
    except OSError:
        pass


def _cycle(run, log, cycle):
    """One sleep -> wake -> sleep. Returns (wake_seconds, resleep_seconds)."""
    read = lambda: log.read_text(errors='replace')
    start = len(read())

    woke = None
    t0 = time.time()
    while time.time() - t0 < 90:
        _poke()
        time.sleep(2)
        seg = read()[start:]
        if 'leaving deep sleep' in seg or 'Host woke up from power save' in seg:
            woke = round(time.time() - t0, 1)
            break
    if woke is None:
        return None, None

    mark = len(read())
    t1 = time.time()
    while time.time() - t1 < 150:
        if 'PMU deep-sleep entry' in read()[mark:]:
            return woke, round(time.time() - t1, 1)
        time.sleep(2)
    return woke, None


@pytest.fixture(scope='module')
def cycles():
    """Run the pair once and drive every cycle inside it. Each cycle costs a
    real sleep and a real wake, so they share one emulator pair."""
    results = []

    def drive(run):
        log = Path(run.log_dir) / 'host.log'
        for c in range(1, CYCLES + 1):
            results.append(_cycle(run, log, c))

    run = emu_pair.run_pair(host_wait_for='PMU deep-sleep entry',
                            emu_seconds=1200, wall_seconds=1200,
                            cp_settle_seconds=150,
                            while_running=drive, wake_port=WAKE_PORT,
                            host_spec=emu_pair.HOST_PS)
    assert run.while_running_exc is None, \
        f'cycle driver raised: {run.while_running_exc}; logs in {run.log_dir}'
    return run, results


def test_host_reaches_deep_sleep(cycles):
    """The idle timer fires and the host actually enters PMU deep sleep.

    run_pair only returns once this marker appears, so reaching the fixture is
    the assertion. Stated explicitly so a failure names the right thing.
    """
    run, _ = cycles
    assert 'PMU deep-sleep entry' in run.host_log, f'logs in {run.log_dir}'


def test_host_wakes_on_traffic(cycles):
    """Every cycle woke on a packet aimed at the host."""
    run, results = cycles
    woke = [w for w, _ in results]
    assert all(w is not None for w in woke), \
        f'wake times {woke}; logs in {run.log_dir}'


def test_host_sleeps_again_after_each_wake(cycles):
    """The timer re-arms: the host goes back to sleep after every wake.

    A host that wakes once and then stays up burns the whole power budget, and
    is the failure a single-shot test would miss.
    """
    run, results = cycles
    reslept = [r for _, r in results]
    assert all(r is not None for r in reslept), \
        f're-sleep times {reslept}; logs in {run.log_dir}'
