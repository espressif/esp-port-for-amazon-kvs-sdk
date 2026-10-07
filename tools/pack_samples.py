#!/usr/bin/env python3
"""Pack per-frame sample files into one indexed file per medium.

Layout, little endian:
    u32 magic 'PKF1'
    u32 count
    count x { u32 offset, u32 length }    offsets are from the start of the file
    payload
"""
import pathlib, struct, sys

MAGIC = 0x31464B50  # 'PKF1'

def pack(sample_dir, pattern, out_path):
    files = sorted(sample_dir.glob(pattern))
    if not files:
        return 0
    header = 8 + 8 * len(files)
    offset, index, blobs = header, [], []
    for f in files:
        b = f.read_bytes()
        index.append((offset, len(b)))
        blobs.append(b)
        offset += len(b)
    with open(out_path, 'wb') as fh:
        fh.write(struct.pack('<II', MAGIC, len(files)))
        for off, ln in index:
            fh.write(struct.pack('<II', off, ln))
        for b in blobs:
            fh.write(b)
    return len(files)

d = pathlib.Path(sys.argv[1])
v = pack(d, 'frame-*.h264', d / 'video.pack')
a = pack(d, 'sample-*.opus', d / 'audio.pack')
print(f'video.pack: {v} frames, {(d/"video.pack").stat().st_size/1024:.0f} KB')
if a:
    print(f'audio.pack: {a} frames, {(d/"audio.pack").stat().st_size/1024:.0f} KB')
