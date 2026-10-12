#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end check of examples/linux_test against a real KVS signaling channel:
# linux_test.elf runs as the master, an aiortc viewer (viewer/viewer.py) records
# what it receives, and the raw H.264 it got is checked against the sample frames.
#
# Required: AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY (+ AWS_SESSION_TOKEN for
# temporary credentials) and a built examples/linux_test/build/linux_test.elf.
# Optional: AWS_DEFAULT_REGION (us-west-2), KVS_CHANNEL_NAME (linux-test-<ts>),
# TEST_DURATION_SEC (45), KVS_FRAMES_DIR, KVS_CA_CERT_PATH, OUT_DIR ($TMPDIR),
# AIORTC_VENV, LINUX_TEST_BUILD_DIR, KEEP_CHANNEL=1 (keep the channel). Fails if
# the viewer fails, the H.264 check fails, or the master does not exit cleanly.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
LINUX_TEST_BUILD_DIR="${LINUX_TEST_BUILD_DIR:-$REPO_ROOT/examples/linux_test/build}"
ELF="$LINUX_TEST_BUILD_DIR/linux_test.elf"

: "${AWS_ACCESS_KEY_ID:?must be set}"
: "${AWS_SECRET_ACCESS_KEY:?must be set}"
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-us-west-2}"
export KVS_CHANNEL_NAME="${KVS_CHANNEL_NAME:-linux-test-$(date +%s)}"
export TEST_DURATION_SEC="${TEST_DURATION_SEC:-45}"
export KVS_FRAMES_DIR="${KVS_FRAMES_DIR:-$REPO_ROOT/amazon-kinesis-video-streams-webrtc-sdk-c/samples}"

if [ -z "${KVS_CA_CERT_PATH:-}" ]; then
    for ca in /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem /etc/pki/tls/certs/ca-bundle.crt; do
        if [ -f "$ca" ]; then KVS_CA_CERT_PATH="$ca"; break; fi
    done
fi
: "${KVS_CA_CERT_PATH:?no CA bundle found, set KVS_CA_CERT_PATH}"
export KVS_CA_CERT_PATH

OUT_DIR="${OUT_DIR:-${TMPDIR:-/tmp}}"
OUT_DIR="${OUT_DIR%/}"
mkdir -p "$OUT_DIR"
export OUT_PATH="$OUT_DIR/linux_test_recording.mkv"
RAW_H264="${OUT_PATH%.*}.h264"
MASTER_LOG="$OUT_DIR/linux_test_master.log"
VIEWER_LOG="$OUT_DIR/linux_test_viewer.log"
rm -f "$OUT_PATH" "$RAW_H264" "$MASTER_LOG" "$VIEWER_LOG"

if [ ! -x "$ELF" ]; then
    echo "error: $ELF not found; build it first:" >&2
    echo "  . \$IDF_PATH/export.sh && cd examples/linux_test && idf.py --preview set-target linux build" >&2
    exit 2
fi

AIORTC_VENV="${AIORTC_VENV:-$OUT_DIR/aiortc_venv}"
if [ ! -x "$AIORTC_VENV/bin/python" ]; then
    echo ">>> creating the viewer venv at $AIORTC_VENV"
    python3 -m venv "$AIORTC_VENV"
    "$AIORTC_VENV/bin/pip" install --quiet --upgrade pip
    "$AIORTC_VENV/bin/pip" install --quiet -r "$HERE/viewer/requirements.txt"
fi

MASTER_PID=""
cleanup() {
    if [ -n "$MASTER_PID" ]; then
        kill -KILL "$MASTER_PID" 2>/dev/null || true   # SIGTERM is blocked by the POSIX port
        wait "$MASTER_PID" 2>/dev/null || true
    fi
    if [ "${KEEP_CHANNEL:-0}" != "1" ]; then
        "$AIORTC_VENV/bin/python" - <<'PY' || true
import os, boto3
kv = boto3.client("kinesisvideo", region_name=os.environ["AWS_DEFAULT_REGION"])
try:
    info = kv.describe_signaling_channel(ChannelName=os.environ["KVS_CHANNEL_NAME"])["ChannelInfo"]
    kv.delete_signaling_channel(ChannelARN=info["ChannelARN"], CurrentVersion=info["Version"])
    print(">>> deleted channel", os.environ["KVS_CHANNEL_NAME"])
except kv.exceptions.ResourceNotFoundException:
    pass
PY
    fi
}
trap cleanup EXIT

echo ">>> channel $KVS_CHANNEL_NAME ($AWS_DEFAULT_REGION), ${TEST_DURATION_SEC} s, output in $OUT_DIR"

# The master outlives the viewer by a few seconds, then must shut down cleanly.
TEST_DURATION_SEC=$((TEST_DURATION_SEC + 20)) "$ELF" >"$MASTER_LOG" 2>&1 &
MASTER_PID=$!
sleep 15   # signaling up on the master

RC=0
"$AIORTC_VENV/bin/python" -u "$HERE/viewer/viewer.py" 2>&1 | tee "$VIEWER_LOG" || RC=$?

# Wait for the master's own exit (bounded): a hang or crash in teardown fails the run.
for _ in $(seq 60); do kill -0 "$MASTER_PID" 2>/dev/null || break; sleep 1; done
if kill -0 "$MASTER_PID" 2>/dev/null; then
    echo ">>> master did not exit on its own"
    RC=1
else
    MASTER_RC=0; wait "$MASTER_PID" || MASTER_RC=$?
    MASTER_PID=""
    [ "$MASTER_RC" -eq 0 ] || { echo ">>> master exited with $MASTER_RC"; RC=1; }
fi

if [ -s "$RAW_H264" ]; then
    echo ">>> raw H.264: $RAW_H264 ($(wc -c <"$RAW_H264" | tr -d ' ') bytes)"
    "$AIORTC_VENV/bin/python" "$HERE/verify_h264_dump.py" "$RAW_H264" "$KVS_FRAMES_DIR" || RC=1
else
    echo ">>> raw H.264 dump missing or empty"
    [ "$RC" -ne 0 ] || RC=1
fi
[ "$RC" -eq 0 ] || echo ">>> FAILED (master log: $MASTER_LOG, viewer log: $VIEWER_LOG)"
exit "$RC"
