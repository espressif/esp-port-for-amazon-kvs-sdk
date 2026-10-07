# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Media capture under ESP-EMU, with no camera, no codec and no network.

Covers everything before the transport: SPIFFS, the sample packs and both
capture interfaces. The `media-selftest` console command pulls frames straight
from the capture interfaces, so this runs without a peer, AWS or a viewer and
fails on the capture path alone. RTP to a real viewer is pytest_e2e_emu.py.
"""

import re

import pytest

import emu_pair

pytestmark = pytest.mark.emu

FRAMES = 10


@pytest.fixture(scope='module')
def selftest():
    run = emu_pair.run_pair(
        host_wait_for='MEDIA-SELFTEST audio',
        emu_seconds=900, wall_seconds=1500,
        # esp-emu types the payload verbatim: without the newline the console
        # never submits the line.
        host_inject=('Streaming example initialized', f'media-selftest {FRAMES}\n'),
    )
    assert not run.host_exited_early, \
        f'host stopped before the selftest reported; logs in {run.log_dir}'
    return run


def _line(run, kind):
    m = re.search(rf'MEDIA-SELFTEST {kind} frames=(\d+) bytes=(\d+) min=(\d+) max=(\d+)',
                  run.host_log)
    assert m, f'no {kind} selftest line; logs in {run.log_dir}'
    return [int(g) for g in m.groups()]


def test_packs_open(selftest):
    """Both packs open and report the frame counts the image was built with."""
    assert 'Opened /spiffs/samples/video.pack: 60 frames' in selftest.host_log
    assert 'Opened /spiffs/samples/audio.pack: 618 frames' in selftest.host_log


def test_video_frames_come_from_the_pack(selftest):
    """H.264 frames vary in size; a fixed size would mean the reader is wrong."""
    frames, total, lo, hi = _line(selftest, 'video')
    assert frames == FRAMES, f'video returned {frames} of {FRAMES}'
    assert total > 0
    assert hi > lo, f'every frame is {hi} B, so the index is not being used'


def test_audio_frames_are_whole_opus_packets(selftest):
    """CBR 64 kbps at 20 ms is 160 B per packet, so every frame is that size."""
    frames, total, lo, hi = _line(selftest, 'audio')
    assert frames == FRAMES, f'audio returned {frames} of {FRAMES}'
    assert lo == hi == 160, f'expected 160 B packets, got {lo}..{hi}'
    assert total == frames * 160
