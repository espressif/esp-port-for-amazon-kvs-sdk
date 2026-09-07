#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
"""
Bit-exact verifier: raw H.264 dump from the python_viewer vs the master's
source samples.

The viewer dumps every depacketized Annex-B access unit before decoding.
RTP packetization does not alter NAL payloads, so every NAL in the dump must
match a NAL in `samples/h264SampleFrames/frame-*.h264` byte-for-byte.

Both inputs are split into NAL units (start codes and trailing zero bytes
stripped) and keyed by (nal_type, sha1). The report gives:
  - matched: dump NALs identical to some source NAL
  - missing_in_source: dump NALs with no source match (a real bug)
  - source_uncaptured: distinct source NALs never seen in the dump (late
    join, packet loss, dropped SEI, or a short run); reported, not a failure

Exit code: 0 if every dump NAL matches, 1 if any does not, 2 if inputs are
missing or empty.

Usage
-----
    verify_h264_dump.py <dump.h264> <samples_dir>
        samples_dir is the path containing `h264SampleFrames/` from the
        upstream KVS SDK.
"""

import hashlib
import os
import sys
from collections import Counter, defaultdict
from glob import glob


NAL_NAMES = {1: "slice", 5: "IDR", 6: "SEI", 7: "SPS", 8: "PPS", 9: "AUD"}


def parse_annexb(buf: bytes) -> list[tuple[int, bytes]]:
    """Return [(nal_type, nal_body)] where nal_body is the NAL header
    byte + RBSP, with start codes stripped from both ends. Each NAL
    body ends at the first byte of the NEXT NAL's start code (not
    after it), and trailing zero bytes (rbsp_trailing / cabac_zero_word
    / encoder padding) are stripped so that the same logical NAL
    produces the same hash regardless of whether it was followed by a
    3-byte vs 4-byte start code."""
    out: list[tuple[int, bytes]] = []
    n = len(buf)
    # boundaries[k] = (sc_start, body_start) for NAL k.
    boundaries: list[tuple[int, int]] = []
    i = 0
    while i < n - 2:
        if buf[i] == 0 and buf[i + 1] == 0:
            if i + 3 < n and buf[i + 2] == 0 and buf[i + 3] == 1:
                boundaries.append((i, i + 4))
                i += 4
                continue
            if buf[i + 2] == 1:
                boundaries.append((i, i + 3))
                i += 3
                continue
        i += 1
    for k, (_, body_start) in enumerate(boundaries):
        body_end = boundaries[k + 1][0] if k + 1 < len(boundaries) else n
        body = buf[body_start:body_end]
        # Drop trailing zero bytes — they're either rbsp_trailing_bits
        # padding or end-of-file padding, never load-bearing for the
        # NAL contents that the depacketizer would have preserved.
        while len(body) > 1 and body[-1] == 0:
            body = body[:-1]
        if not body:
            continue
        nal_type = body[0] & 0x1F
        out.append((nal_type, body))
    return out


def hash_nal(nal: bytes) -> str:
    return hashlib.sha1(nal).hexdigest()


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(f"usage: {argv[0]} <dump.h264> <samples_dir>", file=sys.stderr)
        return 2

    dump_path = argv[1]
    samples_dir = argv[2]

    if not os.path.isfile(dump_path) or os.path.getsize(dump_path) == 0:
        print(f"error: dump '{dump_path}' missing or empty", file=sys.stderr)
        return 2

    sample_glob = os.path.join(samples_dir, "h264SampleFrames", "frame-*.h264")
    sample_files = sorted(glob(sample_glob))
    if not sample_files:
        print(f"error: no frames matched '{sample_glob}'", file=sys.stderr)
        return 2

    # 1) Parse source frames into a per-(nal_type, hash) frequency map.
    source_hashes: dict[tuple[int, str], int] = defaultdict(int)
    source_type_counts: Counter[int] = Counter()
    for fp in sample_files:
        with open(fp, "rb") as fh:
            buf = fh.read()
        for nal_type, body in parse_annexb(buf):
            source_hashes[(nal_type, hash_nal(body))] += 1
            source_type_counts[nal_type] += 1

    # 2) Parse the dump.
    with open(dump_path, "rb") as fh:
        dump_buf = fh.read()
    dump_nals = parse_annexb(dump_buf)
    dump_type_counts: Counter[int] = Counter(t for t, _ in dump_nals)

    # 3) Walk the dump; every NAL must exist in the source set. The master
    #    loops the samples, so a source NAL may legitimately appear many times.
    matched = 0
    unmatched: list[tuple[int, str, int]] = []  # (nal_type, sha, dump_offset)
    seen: set[tuple[int, str]] = set()
    for idx, (nal_type, body) in enumerate(dump_nals):
        key = (nal_type, hash_nal(body))
        if key in source_hashes:
            matched += 1
            seen.add(key)
        else:
            unmatched.append((nal_type, key[1], idx))

    uncaptured = [key for key in source_hashes if key not in seen]
    uncaptured_types: Counter[int] = Counter(t for t, _ in uncaptured)

    # 4) Report.
    print("--- H.264 dump bit-exact verification ---")
    print(f"dump:    {dump_path} ({os.path.getsize(dump_path)} bytes, {len(dump_nals)} NALs)")
    print(f"samples: {samples_dir}/h264SampleFrames ({len(sample_files)} frames, {sum(source_type_counts.values())} NALs)")
    print()
    print("NAL type counts (type | dump | source):")
    for t in sorted(set(dump_type_counts) | set(source_type_counts)):
        print(f"  {NAL_NAMES.get(t, f't{t}'):>6s}  {dump_type_counts.get(t, 0):>6d}  {source_type_counts.get(t, 0):>6d}")
    print()
    print(f"matched: {matched}/{len(dump_nals)} dump NALs are bit-identical to a source NAL")
    print(f"missing_in_source: {len(unmatched)}")
    print(f"source_uncaptured: {len(uncaptured)}/{len(source_hashes)} distinct source NALs never seen in the dump")
    if uncaptured_types:
        by_type = ", ".join(f"{NAL_NAMES.get(t, f't{t}')}={c}" for t, c in sorted(uncaptured_types.items()))
        print(f"  by type: {by_type} (informational, not a failure)")

    if unmatched:
        print()
        print(f"ERROR: {len(unmatched)} NALs in the dump have no byte-identical match in the source set:")
        for nal_type, sha, idx in unmatched[:10]:
            print(f"  dump idx {idx}: type {nal_type} ({NAL_NAMES.get(nal_type, '?')}) sha1={sha}")
        if len(unmatched) > 10:
            print(f"  ... and {len(unmatched) - 10} more")
        print()
        print("This means RTP/SRTP/depacketization mutated NAL payload bytes — a real bug.")
        return 1

    print("PASS — every received NAL is byte-identical to a master-side source NAL.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
