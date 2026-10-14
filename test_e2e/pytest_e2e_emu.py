# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""End to end under ESP-EMU: emulated pair, real AWS, real viewer.

How far this reaches is the point of the test, so it asserts each stage rather
than only the final one. The emulated co-processor raises a SoftAP and reaches
the internet through esp-emu's user-mode NAT, so signaling is real: same channel,
same credentials, same viewer as the hardware suite.

Media is reported, not required. ICE, DTLS and video to the viewer work behind
the NAT only with an esp-emu whose SDIO bridge keeps host-to-slave data moving
(see test_e2e/README.md, "Emulator (no hardware)"); on an older emulator a hard assertion here
would fail on the emulator, not on the firmware.
"""

import re

import pytest

import emu_pair
from kvs_viewer import KvsViewer, run_viewer

pytestmark = pytest.mark.emu


@pytest.fixture(scope='module')
def paired(aws_creds, aws_creds_usable, creds_fragment, kvs_channel, kvs_region, request):
    """The pair, brought up far enough for the co-processor to reach AWS.

    creds_fragment comes first so that test_e2e/sdkconfig.creds exists before the
    co-processor is built; without it the CP carries no keys and cannot sign.

    The viewer runs inside this fixture, not in a test of its own. run_pair kills
    both emulators before it returns, so a viewer started afterwards talks to
    whatever else holds the channel -- a powered board on the bench answers and
    the test passes for the wrong reason.
    """
    def viewer(_run):
        v = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                      duration_s=request.config.getoption('--media-timeout'),
                      connect_timeout_s=request.config.getoption('--connect-timeout'))
        return run_viewer(v)

    return emu_pair.run_pair(
        host_wait_for='READY signal sent successfully',
        emu_seconds=900, wall_seconds=1500,
        cp_wait_for='Signaling connected successfully',
        cp_settle_seconds=240,
        while_running=viewer,
    )


def test_coprocessor_reaches_the_network(paired):
    """Wi-Fi, DHCP and DNS through the emulator's NAT."""
    assert re.search(r'(?:Got IP|sta ip)[:=]?\s*\d+\.\d+\.\d+\.\d+', paired.cp_log), \
        f'co-processor never got an address; logs in {paired.log_dir}'


def test_clock_is_set(paired):
    """SigV4 signs against the clock. At 1970 every signature fails locally."""
    assert 'Time sync completed' in paired.cp_log, \
        f'SNTP never completed, so SigV4 cannot sign; logs in {paired.log_dir}'


def test_signaling_reaches_aws(paired):
    """TLS and SigV4 against the real endpoint."""
    assert 'Date Header from server' in paired.cp_log, \
        f'no HTTPS response from AWS; logs in {paired.log_dir}'
    assert 'token included in the request is expired' not in paired.cp_log, \
        'AWS credentials have expired; refresh test_e2e/sdkconfig.creds'


def test_signaling_walk_completes(paired):
    """The full state walk, the same one the hardware suite asserts."""
    seen = [int(m) for m in re.findall(r'signaling state changed to (\d+)', paired.cp_log)]
    assert 9 in seen or 'Signaling connected successfully' in paired.cp_log, \
        f'walk stopped at {sorted(set(seen))}; logs in {paired.log_dir}'


def test_viewer_offer_reaches_the_host(paired):
    """A real viewer joined the channel while the pair was alive.

    The offer must reach the host across the SDIO bridge, so an answer proves the
    whole split path: viewer -> AWS -> emulated co-processor -> bridge -> host.
    """
    assert paired.while_running_exc is None, \
        f'viewer raised: {paired.while_running_exc}; logs in {paired.log_dir}'
    result = paired.while_running_result
    assert result is not None, f'viewer never ran; logs in {paired.log_dir}'
    print(f'EMU VIEWER: {result.summary()}')
    # A bridge message with both a peer id and a payload is the viewer's offer.
    # Control traffic has neither, so the two sides cannot satisfy this alone.
    crossed = re.search(r'peer_id=\S+, payload_len=[1-9]', paired.host_log)
    assert crossed, \
        f'no viewer offer crossed the bridge; logs in {paired.log_dir}'
    # crossed establishes the offer reached our device; this proves it answered.
    assert result.answer_received, \
        f'offer arrived but the host never answered; logs in {paired.log_dir}'


def test_media_flows_to_the_viewer(paired):
    """Measured, not required: it depends on the esp-emu version as well as on
    the firmware, so a missing stream is a skip that names what the viewer saw."""
    result = paired.while_running_result
    if not result or not result.video_packets:
        pytest.skip(f'no media through the NAT: {result.summary() if result else "no viewer"}')
    print(f'EMU MEDIA: {result.video_packets} video, {result.audio_packets} audio')
