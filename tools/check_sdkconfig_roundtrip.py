#!/usr/bin/env python3
"""Report sdkconfig.defaults assignments that the build silently dropped.

kconfgen ignores an assignment to a symbol that has no prompt, or whose
"depends on" is unsatisfied, and it does not warn. The assignment looks
applied but the build never sees it. This compares each assignment in the
defaults files against the generated sdkconfig.h and lists the ones that did
not survive.

Usage:
  check_sdkconfig_roundtrip.py <project_dir> <build_dir> [more build dirs]
  check_sdkconfig_roundtrip.py --defaults a.defaults,b.defaults <project_dir> <build_dir>

Build the project first; the build directory must hold config/sdkconfig.h.

Without --defaults the tool assumes the plain idf.py chain (sdkconfig.defaults
then sdkconfig.defaults.<target>). Pass --defaults to check a build made with an
explicit SDKCONFIG_DEFAULTS list, such as a board overlay; give the same files in
the same order the build used.
"""
import re
import sys
from pathlib import Path

# Symbols owned by dependencies, not by this repo. Listed so a real regression
# does not hide among them.
IGNORE = {
    # set-target owns these; a defaults file cannot change them
    "IDF_TARGET", "IDF_TARGET_ESP32C6", "IDF_TARGET_ESP32C5", "IDF_TARGET_ESP32P4",
    # set per target by the partition-table overlay
    "PARTITION_TABLE_CUSTOM_FILENAME", "PARTITION_TABLE_FILENAME",
    # esp_hosted's own example defaults
    "IPERF_REPORT_TASK_PRIORITY", "IPERF_TRAFFIC_TASK_PRIORITY",
    "WIFI_CMD_BASIC_ONLY", "ESP_HOST_DEV_BOARD_P4_C5_CORE",
    # pre-existing app-level symbols with no Kconfig definition
    "ENABLE_DATA_CHANNEL", "ENABLE_SIGNALLING_ONLY",
    "MB_SERIAL_TASK_STACK_SIZE", "MBEDTLS_DYNAMIC_FREE_PEER_CERT",
    "SUPPORT_STATIC_ALLOCATION",
}

ASSIGN = re.compile(r'^CONFIG_([A-Za-z0-9_]+)=(.*)$')
DEFINE = re.compile(r'^#define CONFIG_([A-Za-z0-9_]+) (.*)$')
# kconfgen spells a renamed symbol as an alias to its new name.
ALIAS = re.compile(r'^CONFIG_[A-Za-z0-9_]+$')


def load_generated(build_dir):
    header = Path(build_dir) / "config" / "sdkconfig.h"
    if not header.is_file():
        sys.exit(f"no generated config at {header}; build the project first")
    out = {}
    for line in header.read_text().splitlines():
        m = DEFINE.match(line)
        if m:
            out[m.group(1)] = m.group(2).strip()
    # Resolve rename aliases so an assignment to the old name is judged against
    # the value the new name actually got.
    for name, value in list(out.items()):
        if ALIAS.match(value):
            target = value[len("CONFIG_"):]
            if target in out:
                out[name] = out[target]
    return out


def wanted(value):
    """Normalise a defaults value to how sdkconfig.h would spell it."""
    value = value.strip()
    if value == "y":
        return "1"
    if value.startswith('"'):
        return value
    return value


def check(defaults_files, generated):
    """Later defaults files override earlier ones, so judge each symbol only
    against its last assignment."""
    last = {}
    for path in defaults_files:
        for lineno, line in enumerate(path.read_text().splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = ASSIGN.match(line)
            if not m:
                continue
            last[m.group(1)] = (path.name, lineno, m.group(2))

    dropped = []
    for name, (fname, lineno, value) in sorted(last.items()):
        if name in IGNORE:
            continue
        if not value.strip():
            dropped.append((fname, lineno, name, value, "empty value"))
            continue
        if value.strip() == "n":
            # "=n" is spelled as an absent define; nothing to compare.
            if name in generated:
                dropped.append((fname, lineno, name, value,
                                f"build has {generated[name]}"))
            continue
        got = generated.get(name)
        if got is None:
            dropped.append((fname, lineno, name, value, "never reached the build"))
        elif got != wanted(value):
            dropped.append((fname, lineno, name, value, f"build has {got}"))
    return dropped


def main():
    argv = sys.argv[1:]
    explicit = None
    if argv and argv[0] == "--defaults":
        if len(argv) < 2:
            sys.exit(__doc__)
        explicit = [a.strip() for a in argv[1].split(",") if a.strip()]
        argv = argv[2:]
    if len(argv) < 2:
        sys.exit(__doc__)
    project = Path(argv[0])
    files = sorted(project.glob("sdkconfig.defaults*"))
    if not files:
        sys.exit(f"no sdkconfig.defaults* under {project}")

    failures = 0
    for build_dir in argv[1:]:
        generated = load_generated(build_dir)
        target = generated.get("IDF_TARGET", "?")
        # A per-target defaults file only applies to its own target.
        chip = target.strip('"')
        if explicit:
            applicable = []
            for name in explicit:
                f = project / name
                if not f.is_file():
                    sys.exit(f"no such defaults file: {f}")
                applicable.append(f)
        else:
            # idf.py reads sdkconfig.defaults then sdkconfig.defaults.<target>.
            # A board overlay (sdkconfig.defaults.<board>.<target>) is only read
            # when named explicitly, so it is not part of this build.
            applicable = [f for f in files
                          if f.name in ("sdkconfig.defaults", f"sdkconfig.defaults.{chip}")]
        dropped = check(applicable, generated)
        print(f"\n{project.name} / {build_dir} (target {target})")
        if not dropped:
            print("  every assignment reached the build")
            continue
        failures += len(dropped)
        for fname, lineno, name, value, why in dropped:
            print(f"  {fname}:{lineno}: CONFIG_{name}={value} -- {why}")

    print()
    if failures:
        print(f"{failures} assignment(s) silently dropped")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
