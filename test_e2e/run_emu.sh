#!/usr/bin/env bash
# Launch the emulated P4 + C6 pair for the end-to-end tests.
#
# ESP-EMU models the SDIO transport between two emulator processes, so the P4 host
# and the C6 co-processor each run as their own `esp-emu`, joined by a Unix socket.
# The slave must be listening before the host connects.
#
#   ./run_emu.sh start   # bring both up in the background
#   ./run_emu.sh stop    # tear them down
#   ./run_emu.sh logs    # follow both logs
#
# Env overrides: ESP_EMU (path to the binary; default: esp-emu on PATH),
# EMU_RUNDIR (logs + socket).
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ESP_EMU="${ESP_EMU:-$(command -v esp-emu || true)}"
RUNDIR="${EMU_RUNDIR:-/tmp/kvs-emu}"
SOCK="$RUNDIR/hosted.sock"

CP_DIR="$REPO/examples/network_adapter/build_emu"
HOST_DIR="$REPO/examples/webrtc_classic/build_emu"

die() { echo "error: $*" >&2; exit 1; }

check() {
  [ -n "$ESP_EMU" ] && [ -x "$ESP_EMU" ] || die "esp-emu not found${ESP_EMU:+ at $ESP_EMU}
  Put esp-emu on PATH, or point ESP_EMU at the binary.
  See test_e2e/README.md 'Emulator (no hardware)' for the version it needs."
  for f in "$CP_DIR/merged_flash.bin" "$HOST_DIR/merged_flash.bin"; do
    [ -f "$f" ] || die "missing $f -- see test_e2e/README.md 'Emulator (no hardware)' for the build steps"
  done
}

start() {
  check
  mkdir -p "$RUNDIR"
  rm -f "$SOCK"

  # The C6 goes first so its socket is listening. --net user gives it NAT'd
  # networking, which is how the emulated pair reaches AWS at all.
  "$ESP_EMU" --chip esp32c6 \
      --firmware "$CP_DIR/merged_flash.bin" \
      --elf "$CP_DIR/network_adapter.elf" \
      --hosted "bridge:slave:$SOCK" \
      --net user > "$RUNDIR/c6.log" 2>&1 &
  echo $! > "$RUNDIR/c6.pid"

  # Give the slave a moment to bind before the host dials in.
  for _ in $(seq 1 50); do [ -S "$SOCK" ] && break; sleep 0.1; done
  [ -S "$SOCK" ] || { cat "$RUNDIR/c6.log"; die "C6 never created $SOCK"; }

  "$ESP_EMU" --chip esp32p4 \
      --firmware "$HOST_DIR/merged_flash.bin" \
      --elf "$HOST_DIR/webrtc_classic.elf" \
      --hosted "bridge:host:$SOCK" > "$RUNDIR/p4.log" 2>&1 &
  echo $! > "$RUNDIR/p4.pid"

  echo "C6 pid $(cat "$RUNDIR/c6.pid")  log $RUNDIR/c6.log"
  echo "P4 pid $(cat "$RUNDIR/p4.pid")  log $RUNDIR/p4.log"
}

stop() {
  for c in p4 c6; do
    if [ -f "$RUNDIR/$c.pid" ]; then
      kill "$(cat "$RUNDIR/$c.pid")" 2>/dev/null || true
      rm -f "$RUNDIR/$c.pid"
    fi
  done
  rm -f "$SOCK"
  echo "stopped"
}

case "${1:-start}" in
  start) start ;;
  stop)  stop ;;
  logs)  tail -f "$RUNDIR/c6.log" "$RUNDIR/p4.log" ;;
  *)     die "usage: $0 {start|stop|logs}" ;;
esac
