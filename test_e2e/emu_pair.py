# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0

"""Run the streaming_only host and signaling_only co-processor together under
esp-emu, joined by esp-emu's SDIO bridge.

Two rules this harness enforces, both learned the hard way:

  - A slave is never reused. esp-emu's bridge is a length-prefixed stream; a
    host killed mid-transaction leaves the framing desynced, and the next host
    to connect sees a burst of "EMU-FAULT ... frame incomplete" and reads 0xFF
    for every register. Every run gets a fresh socket, a fresh CP and a fresh
    host.
  - Every test asserts the EMU-FAULT count is zero. That line separates a real
    firmware finding from harness breakage.

Builds are cached by build directory, so the first run pays for two images and
later runs do not.
"""

import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
# Same lookup as run_emu.sh so both agree on where the binary lives: $ESP_EMU,
# else esp-emu on PATH. Empty when neither is set; run_pair() says so.
EMU = Path(os.environ.get("ESP_EMU") or shutil.which("esp-emu") or "")
# The ESP-IDF to build with is the one the shell exported (. $IDF_PATH/export.sh).
IDF = Path(os.environ["IDF_PATH"]) if os.environ.get("IDF_PATH") else None

# (project, target, build dir, defaults chain). The .emu overlay goes last; it
# carries the emulator-only deltas and explains each one in place.
# AWS credentials are appended when test_e2e/sdkconfig.creds exists. Without them
# the CP cannot sign a request and every assertion above the bridge is vacuous.
# build() looks for the file when it runs, not at import: the creds_fragment fixture
# writes it during the session, after this module has been imported.
_CREDS = REPO / "test_e2e/sdkconfig.creds"

CP = (
    REPO / "examples/signaling_only",
    "esp32c6",
    "build_emu",
    "sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.ci.e2e;sdkconfig.ci.emu",
)
# Built for the ESP32-P4 Function EV Board v1.6. On the plain per-target defaults
# app_main returns at ~5 s without signaling, so no READY is ever sent.
HOST = (
    REPO / "examples/streaming_only",
    "esp32p4",
    "build_emu",
    "sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.ci.e2e;sdkconfig.ci.emu",
)

ANSI = re.compile(r"\x1b\[[0-9;]*m")


@dataclass
class Run:
    """One pair run: the two firmware logs, plus the emulator's own output."""

    cp_log: str = ""
    host_log: str = ""
    log_dir: Path = None
    host_exited_early: bool = False
    while_running_result: object = None
    while_running_exc: object = None

    def faults(self) -> list:
        """esp-emu's own EMU-FAULT lines, from either side."""
        return [
            ln
            for ln in (self.cp_log + self.host_log).splitlines()
            if "EMU-FAULT" in ln
        ]

    def cp_has(self, needle: str) -> bool:
        return needle in self.cp_log

    def host_has(self, needle: str) -> bool:
        return needle in self.host_log

    def host_after(self, first: str, second: str) -> bool:
        """True when both markers are present and `first` precedes `second`."""
        a = self.host_log.find(first)
        b = self.host_log.find(second)
        return a != -1 and b != -1 and a < b


def _idf(project: Path, args: list, log: Path) -> None:
    assert IDF is not None, "IDF_PATH is not set; source an ESP-IDF export.sh first"
    cmd = f'. {IDF}/export.sh >/dev/null 2>&1; idf.py {" ".join(args)}'
    with log.open("a") as fh:
        subprocess.run(
            ["bash", "-c", cmd], cwd=project, stdout=fh, stderr=subprocess.STDOUT,
            check=True,
        )


def _cached_target(out: Path):
    """IDF_TARGET the build dir was configured for, or None if it has none."""
    cache = out / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(errors="replace").splitlines():
        if line.startswith("IDF_TARGET:"):
            return line.split("=", 1)[1].strip()
    return None


def build(spec: tuple) -> tuple:
    """Build and merge one image. Returns (merged_flash.bin, app.elf).

    The build always runs. It used to return early whenever merged_flash.bin
    existed, which meant a source change never reached the emulator: runs kept
    using whatever image happened to be on disk, and a test could pass or fail
    against firmware from days earlier. ninja is incremental, so an unchanged
    tree costs a couple of seconds.
    """
    project, target, build_dir, defaults = spec
    # Every co-processor build, including variants of CP (the Wi-Fi mismatch
    # overlay of the re-init and offer-wake tests), needs the credentials.
    if project == CP[0]:
        if _CREDS.exists():
            defaults += f";{_CREDS}"
        else:
            print(f"emu_pair: no {_CREDS.name}; the co-processor is built without AWS "
                  f"credentials and cannot sign a signaling request")
    out = project / build_dir
    merged = out / "merged_flash.bin"

    log = Path(tempfile.gettempdir()) / f"emu_build_{project.name}.log"
    log.write_text("")
    # The sdkconfig lives in the build dir: the root one belongs to the hardware
    # builds. Regenerate it from the defaults every time, since a stale sdkconfig
    # wins over them. set-target does that but also fullcleans, so the whole tree
    # would rebuild each run; use it only when the build dir has no target or a
    # different one, and reconfigure otherwise.
    sdkconfig = out / "sdkconfig"
    sdkconfig.unlink(missing_ok=True)
    action = ["reconfigure"] if _cached_target(out) == target else ["set-target", target]
    _idf(project, ["-B", build_dir, f'-DSDKCONFIG="{sdkconfig}"',
                   f'-DSDKCONFIG_DEFAULTS="{defaults}"', *action], log)
    _idf(project, ["-B", build_dir, f'-DSDKCONFIG="{sdkconfig}"', "build"], log)
    subprocess.run(
        ["bash", "-c",
         f". {IDF}/export.sh >/dev/null 2>&1; "
         f"python -m esptool --chip {target} merge_bin -o merged_flash.bin @flash_args"],
        cwd=out, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
    )
    elfs = list(out.glob("*.elf"))
    assert merged.is_file() and elfs, f"build produced no image; see {log}"
    return merged, elfs[0]


def _spawn(args: list, out_path: Path):
    fh = out_path.open("w")
    proc = subprocess.Popen(
        [str(EMU)] + args, stdout=fh, stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL, start_new_session=True,
        env={**os.environ, "RUST_LOG": "info"},
    )
    return proc, fh


def _read(path: Path) -> str:
    try:
        return ANSI.sub("", path.read_text(errors="replace"))
    except FileNotFoundError:
        return ""


def _stop(proc) -> None:
    if proc.poll() is None:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        proc.wait(timeout=30)


def run_pair(host_wait_for: str, emu_seconds: int = 900,
             wall_seconds: int = 900, host_inject: tuple = None,
             cp_inject: tuple = None,
             wifi_ssid: str = "myssid", wifi_password: str = "mypassword",
             cp_wait_for: str = None, cp_settle_seconds: int = 240,
             while_running=None) -> Run:
    """Start a fresh CP, then a fresh host, and wait until `host_wait_for`
    appears in the host log or the wall clock runs out.

    cp_inject: same as host_inject, but for the co-processor. Needed to drive
    anything the CP owns, such as Wi-Fi provisioning.
    host_inject: optional (trigger, payload) fed to the host's UART RX, the way
    a person would type a console command. Used to drive the deep-sleep path.

    cp_wait_for: keep running after the host marker until this appears in the CP
    log. The two sides do not advance at the same rate - the host can send before
    the co-processor's bridge is up - so stopping the moment the host is done
    truncates the CP's half of the exchange.
    while_running: called once both markers are satisfied, before teardown, with
    the Run. Anything that must talk to the live device belongs here. A viewer run
    after this function returns reaches a dead emulator, and any answer it sees
    came from some other master on the same channel.
    """
    assert EMU.is_file(), \
        "esp-emu not found: set ESP_EMU to the binary or put esp-emu on PATH"
    cp_img, cp_elf = build(CP)
    host_img, host_elf = build(HOST)

    # Kept, not temp: the two logs are the only record of why an assertion failed.
    root = Path(os.environ.get("KVS_EMU_LOGS", tempfile.gettempdir())) / "kvs_emu"
    work = root / f"{int(time.time())}_{host_wait_for.split()[0].lower()}"
    work.mkdir(parents=True, exist_ok=True)
    sock = work / "pair.sock"
    run = Run()
    cp_out, host_out = work / "cp.log", work / "host.log"
    cp_proc = host_proc = None
    cp_fh = host_fh = None
    try:
        # esp-emu raises a SoftAP with these; sdkconfig.ci.emu compiles the same
        # pair into the CP, so it associates unattended.
        cp_args = ["--chip", "esp32c6", "--firmware", str(cp_img), "--elf", str(cp_elf),
                   "--hosted", f"bridge:slave:{sock}", "--net", "user",
                   "--wifi-ssid", wifi_ssid, "--wifi-password", wifi_password,
                   "--timeout", f"{emu_seconds}s"]
        if cp_inject:
            trigger, payload = cp_inject
            cp_args += ["--inject-on", trigger, "--inject", payload]
        cp_proc, cp_fh = _spawn(cp_args, cp_out)
        deadline = time.time() + 60
        while not sock.is_socket():
            assert time.time() < deadline, "CP never opened the bridge socket"
            assert cp_proc.poll() is None, f"CP exited early:\n{_read(cp_out)[-2000:]}"
            time.sleep(0.5)
        # No extra delay here. The CP sends its slave-up event once, early; if the
        # host is not yet enumerating, esp-emu's slave zero-fills the transfer
        # ("served 54 of 1536 CMD53 bytes") and the host sees corrupt framing.

        host_args = [
            "--chip", "esp32p4", "--firmware", str(host_img), "--elf", str(host_elf),
            "--hosted", f"bridge:host:{sock}", "--timeout", f"{emu_seconds}s",
        ]
        if host_inject:
            trigger, payload = host_inject
            host_args += ["--inject-on", trigger, "--inject", payload]
        host_proc, host_fh = _spawn(host_args, host_out)

        deadline = time.time() + wall_seconds
        while time.time() < deadline:
            if host_wait_for in _read(host_out):
                break
            if host_proc.poll() is not None:
                # The host emulator ending before the marker is a result, not a
                # wait condition: record it so the failure names it.
                run.host_exited_early = True
                break
            time.sleep(2)

        if cp_wait_for:
            settle = time.time() + cp_settle_seconds
            while time.time() < settle and cp_wait_for not in _read(cp_out):
                if cp_proc.poll() is not None:
                    break
                time.sleep(2)

        # A viewer hiccup must not erase the network evidence already in run:
        # raising here errors the fixture and takes every assertion down with it.
        if while_running and not run.host_exited_early:
            try:
                run.while_running_result = while_running(run)
            except Exception as exc:
                run.while_running_exc = exc
    finally:
        for proc in (host_proc, cp_proc):
            if proc:
                _stop(proc)
        for fh in (cp_fh, host_fh):
            if fh:
                fh.close()
        run.cp_log = _read(cp_out)
        run.host_log = _read(host_out)
        run.log_dir = work
    return run
