# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""A viewer keeps its video when media-selftest runs during the session.

Both are file-capture sources reading the one sample pack, and only one of them
may feed the sink registry. The selftest is typed on the host console when the
offer reaches the host, so it is running, and owns the sink pump, when the session
starts its own source once the peer connection is built, tens of milliseconds
later. (The answer goes out only after that source has started, so it cannot be
the trigger.) When the selftest ends the pump has to pass
to the session's source; before that handover existed the viewer got no more
video for the rest of the session. The selftest pulls through get_frame() with its
own buffer and read position, so it still gets every frame it asks for.

Needs AWS credentials and a viewer, like pytest_e2e_emu.py.
"""

import re

import pytest

import emu_pair
from kvs_viewer import KvsViewer, run_viewer

pytestmark = pytest.mark.emu

# 10 s of pull at the file source's default 25 fps: still running when the
# session's source starts, and finished with time left in the session (about
# 20 s of emulated time on esp-emu) to see the handover and the video after it.
SELFTEST_FRAMES = 250
VIEWER_MEDIA_S = 60

SELFTEST_VIDEO = re.compile(r'MEDIA-SELFTEST video frames=(\d+) bytes=(\d+)')
SEND_FPS = re.compile(r'DIAG frame_cb: send_v_fps=(\d+)')
OFFER_RECEIVED = 'Deserialized message: type=0'
SESSION_MEDIA_START = 'Starting global media threads for first session'
SOURCE_STARTED = 'File-based video capture started'
PUMP_TAKEN = 'Another file video source owns the sink pump'
PUMP_HANDED_OVER = 'File video sink pump handed over to the remaining source'


@pytest.fixture(scope='module')
def session(aws_creds, aws_creds_usable, creds_fragment, kvs_channel, kvs_region, request):
    """One viewer session, with media-selftest started when the offer arrives.

    creds_fragment comes first so test_e2e/sdkconfig.creds exists before the
    co-processor is built. The viewer runs inside run_pair, which stops both
    emulators before it returns.
    """
    def viewer(_run):
        v = KvsViewer(kvs_channel, kvs_region, credentials=aws_creds,
                      duration_s=VIEWER_MEDIA_S,
                      connect_timeout_s=request.config.getoption('--connect-timeout'))
        return run_viewer(v)

    return emu_pair.run_pair(
        host_wait_for='READY signal sent successfully',
        emu_seconds=900, wall_seconds=1500,
        cp_wait_for='Signaling connected successfully',
        cp_settle_seconds=240,
        # esp-emu types the payload verbatim: without the newline the console
        # never submits the line.
        host_inject=(OFFER_RECEIVED, f'media-selftest {SELFTEST_FRAMES}\n'),
        while_running=viewer,
    )


def _after(log, marker):
    """The part of the log after the first occurrence of marker, or ''."""
    i = log.find(marker)
    return log[i + len(marker):] if i >= 0 else ''


def test_viewer_got_the_answer(session):
    assert session.while_running_exc is None, \
        f'viewer raised: {session.while_running_exc}; logs in {session.log_dir}'
    result = session.while_running_result
    assert result and result.answer_received, \
        f'no answer: {result.summary() if result else "no viewer"}; logs in {session.log_dir}'


def test_selftest_owned_the_pump_when_the_session_started(session):
    # Without this ordering the test passes for the easy case, where the session's
    # source is first and the selftest only pulls. So it is the session's own
    # start, not the selftest's, that has to find the pump taken.
    start = _after(session.host_log, SESSION_MEDIA_START)
    end = start.find(SOURCE_STARTED)
    assert end >= 0 and PUMP_TAKEN in start[:end], \
        f'the session source started without the selftest holding the pump; ' \
        f'logs in {session.log_dir}'


def test_selftest_got_every_frame(session):
    m = SELFTEST_VIDEO.search(session.host_log)
    assert m, f'no MEDIA-SELFTEST video line; logs in {session.log_dir}'
    assert int(m.group(1)) == SELFTEST_FRAMES, \
        f'selftest got {m.group(1)} of {SELFTEST_FRAMES} frames; logs in {session.log_dir}'


def test_pump_passes_to_the_session_when_the_selftest_ends(session):
    m = SELFTEST_VIDEO.search(session.host_log)
    assert m, f'no MEDIA-SELFTEST video line; logs in {session.log_dir}'
    assert PUMP_HANDED_OVER in session.host_log[m.end():], \
        f'the pump was not handed to the session source; logs in {session.log_dir}'


def test_video_is_sent_after_the_selftest_ends(session):
    tail = _after(session.host_log, PUMP_HANDED_OVER)
    rates = [int(r) for r in SEND_FPS.findall(tail)]
    assert any(r > 0 for r in rates), \
        f'no video sent after the selftest ended (send_v_fps after the handover: ' \
        f'{rates}); logs in {session.log_dir}'


def test_viewer_decodes_video(session):
    result = session.while_running_result
    if not result or not result.video_packets:
        pytest.skip(f'no media through the NAT: {result.summary() if result else "no viewer"}')
    print(f'EMU SELFTEST+STREAM: {result.summary()}')
    assert result.frames_video > 0, \
        f'video packets arrived but none decoded: {result.summary()}; logs in {session.log_dir}'
