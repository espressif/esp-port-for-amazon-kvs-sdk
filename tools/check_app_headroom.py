#!/usr/bin/env python3
"""Fail when an app image leaves less than a minimum of its smallest app slot free.

A device in the field cannot grow its app partitions: a hosted or OTA update keeps
the partition table it shipped with. So the co-processor image must keep room for
the next esp_hosted or ESP-IDF update. The build itself fails only when the image
no longer fits at all.

Usage:
  check_app_headroom.py <build_dir> <min_free_bytes>

Build the project first. The build directory must hold project_description.json,
the app image and partition_table/partition-table.bin.
"""

import json
import os
import subprocess
import sys


def parse_size(text):
    """A gen_esp32part size: hex or decimal bytes, or a K or M suffix."""
    for suffix, scale in (('K', 1024), ('M', 1024 * 1024)):
        if text.endswith(suffix):
            return int(text[:-1], 0) * scale
    return int(text, 0)


def app_slot_sizes(idf_path, table_bin):
    """Sizes of the app partitions in a binary partition table."""
    out = subprocess.run(
        [sys.executable, os.path.join(idf_path, 'components', 'partition_table', 'gen_esp32part.py'),
         table_bin],
        check=True, capture_output=True, text=True).stdout
    sizes = []
    for line in out.splitlines():
        fields = [f.strip() for f in line.split(',')]
        if len(fields) >= 5 and not fields[0].startswith('#') and fields[1] == 'app':
            sizes.append(parse_size(fields[4]))
    return sizes


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build_dir, min_free = sys.argv[1], int(sys.argv[2], 0)
    idf_path = os.environ['IDF_PATH']

    with open(os.path.join(build_dir, 'project_description.json')) as f:
        desc = json.load(f)
    app_bin = os.path.join(build_dir, desc['app_bin'])
    slots = app_slot_sizes(idf_path, os.path.join(build_dir, 'partition_table', 'partition-table.bin'))
    if not slots:
        sys.exit(f'{build_dir}: no app partition in the partition table')

    image, slot = os.path.getsize(app_bin), min(slots)
    free = slot - image
    print(f'{desc["app_bin"]}: {image} bytes in a {slot}-byte slot, {free} bytes ({free * 100 // slot}%) free, '
          f'minimum {min_free}')
    if free < min_free:
        sys.exit(f'{desc["app_bin"]} leaves {free} bytes free, below the {min_free}-byte minimum. '
                 'Trim the build before the slot runs out: devices in the field cannot grow it.')


if __name__ == '__main__':
    main()
