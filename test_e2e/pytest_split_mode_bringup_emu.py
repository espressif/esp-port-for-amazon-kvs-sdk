"""Bring-up checks for the split pair (streaming_only P4 + signaling_only C6)
under ESP-EMU. This is not an end-to-end test.

What it proves: both halves boot, take the roles and features they are configured
for, agree on the SDIO datapath, and carry one signaling frame across the bus.
Every assertion is a log marker from that bring-up.

What it does not touch, because the emulator cannot: joining an AP, reaching KVS,
the signaling state walk, TLS or SigV4, ICE, and any media. esp-emu's user-mode
NAT does not carry the co-processor's SNTP traffic, so the clock stays at 1970 and
every SigV4 signature fails locally in about a millisecond without reaching the
network; ICE never forms behind that NAT either. The end-to-end assertions live in
pytest_split_mode.py, which needs hardware and AWS.

Worth having anyway: this is the layer an esp_hosted or IDF change breaks first,
and it runs with no hardware, no credentials and no bench.

    pytest pytest_split_mode_bringup_emu.py --emu -v

Two esp-emu processes joined by the SDIO bridge, the same arrangement as
run_emu.sh. Also out of reach here: EH_HOST_PORT_DMA_PREFER_SPIRAM is a PSRAM
cache-coherency property that esp-emu does not model at all.
"""

import pytest

import emu_pair

pytestmark = pytest.mark.emu


@pytest.fixture(scope='module')
def pair():
    """One bring-up shared by the marker assertions: two emulator runs total,
    not one per test."""
    run = emu_pair.run_pair(host_wait_for='READY signal sent successfully',
                            emu_seconds=900, wall_seconds=1500,
                            cp_wait_for='msg_id 4096')
    assert not run.host_exited_early, \
        f'host emulator stopped before sending READY; logs in {run.log_dir}'
    return run


def test_bridge_is_clean(pair):
    """esp-emu reported no bridge faults.

    Asserted first because every other result here is meaningless if the bus was
    feeding the firmware 0xFF. In practice this catches a reused slave: the
    bridge is a length-prefixed stream, so a host killed mid-transaction leaves
    the framing desynced for whoever connects next.
    """
    assert pair.faults() == [], f'logs in {pair.log_dir}'


def test_coprocessor_role_and_power_save(pair):
    """The C6 comes up in 3.x co-processor role with host-power-save enabled by
    the application rather than by feature auto-init.

    CP_FEAT_HOST_PS_AUTO_INIT is off on purpose: host_ps_integration_init() must
    enable the feature itself so the four sleep callbacks are attached. If
    auto-init came back, the feature would still initialise and
    'coprocessor part enabled' would be missing.
    """
    assert pair.cp_has('ESP-Hosted Firmware version :: EH-3.')
    assert pair.cp_has('host power save: coprocessor part enabled')
    assert pair.cp_has('Light sleep initialized successfully')
    assert pair.cp_has('Peer data transfer extension initialized')


def test_host_features_and_netif_owner(pair):
    """The host builds the features the bridge and recovery paths need, and
    takes the netif network split created instead of making a second one.

    A second esp_netif_create_default_wifi_sta() returns NULL for a duplicate
    key and asserts inside wifi_default.c. This is the guard on that regression.
    """
    for feature in ("'heartbeat'", "'peer_data'", "'power_save'", "'nw_split'"):
        assert pair.host_has(feature), f'host did not init {feature}'
    assert pair.host_has('Armed wakeup GPIO')
    assert pair.host_has('Using WiFi STA netif from network split')
    assert not pair.host_has('Failed to create default WiFi STA netif')


def test_both_halves_agree_on_the_sdio_datapath(pair):
    """Host and co-processor negotiated the link rather than aborting on a
    mismatch.

    The two sides pick their SDIO datapath independently, and esp_hosted refuses
    to run if they disagree:

        eh_init_evt: SDIO mode: slave=streaming host=packet
        assert failed: sdio_mode_check eh_host_mcu_transport_init_event.c:159

    which is what happens when a host overlay forces packet mode against the
    co-processor's SW_AGGR default. Asserting the firmware versions match is the
    same idea one layer up: both halves must be the same generation.
    """
    assert not pair.host_has('SDIO mode mismatch'), \
        'host and co-processor disagreed on the SDIO datapath'
    assert pair.host_has('TRANSPORT_RX_ACTIVE') or pair.host_has('Card init success'), \
        'the SDIO card never enumerated'
    assert pair.host_has('(match)'), \
        'host and co-processor report different esp_hosted versions'


def test_signaling_crosses_the_bus(pair):
    """A signaling message leaves the host, crosses SDIO, and is delivered to the
    co-processor's peer-data dispatcher.

    The whole outbound path in one assertion: serialise, chunk, hand to hosted
    peer data, cross the bus, dispatch on msg_id 0x1000 (4096).

    It stops at delivery rather than demanding the co-processor act on the
    message. The halves boot at different speeds -- the host sends READY around
    7 s while the co-processor registers its bridge callbacks around 13 s -- so
    whether a handler is attached when the frame lands is a race. Tighten this to
    'Received READY signal from P4' once the bridge re-arms on transport-up.
    """
    assert pair.host_has('READY signal sent successfully')
    assert pair.cp_has('msg_id 4096'), \
        "co-processor never saw the host's 0x1000 frame - the bus did not carry it"
