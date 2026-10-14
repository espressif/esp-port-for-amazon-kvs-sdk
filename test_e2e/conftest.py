# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""Fixtures for the end-to-end streaming tests.

Everything here is gated on --e2e so a plain `pytest` collection (and the existing
test_host CI job) stays untouched.
"""

import logging
import os
import re
import time
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
TEST_E2E_DIR = Path(__file__).resolve().parent
CREDS_FRAGMENT = TEST_E2E_DIR / 'sdkconfig.creds'


def pytest_addoption(parser):
    group = parser.getgroup('kvs-e2e')
    group.addoption('--e2e', action='store_true', default=False,
                    help='run the end-to-end streaming tests (needs hardware, Wi-Fi, AWS)')
    group.addoption('--emu', action='store_true', default=False,
                    help='run the emulator tests (needs esp-emu; no hardware, no AWS)')
    group.addoption('--kvs-channel', default=os.getenv('AWS_KVS_CHANNEL_NAME', 'ESP32Channel'),
                    help='KVS signaling channel name the device is configured with')
    group.addoption('--kvs-region', default=os.getenv('AWS_DEFAULT_REGION', 'us-east-1'))
    group.addoption('--allow-relay', action='store_true', default=False,
                    help='accept a TURN-relayed candidate pair instead of failing')
    group.addoption('--media-timeout', type=float, default=20.0,
                    help='seconds to keep pulling frames once connected')
    group.addoption('--connect-timeout', type=float, default=90.0,
                    help='seconds to wait for signaling + ICE + DTLS')
    group.addoption('--manual', action='store_true', default=False,
                    help='run tests marked @pytest.mark.manual. They need a person or an '
                         'instrument (power meter, rewiring, a phone app), so automation '
                         'must never pass this.')
    group.addoption('--cp-port', default=None,
                    help='Serial port of the co-processor console. Required when the '
                         'host firmware is built without CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR, '
                         'i.e. when it does not re-print the CP log with a [SLAVE] prefix.')
    group.addoption('--max-video-loss', type=float, default=1.0,
                    help='fail if inbound video packet loss exceeds this fraction. '
                         'Defaults to 1.0 (report only) because the current baseline '
                         'has not been measured with per-SSRC counting yet; '
                         'set it once you have a baseline worth defending.')


def pytest_configure(config):
    config.addinivalue_line('markers', 'e2e: end-to-end streaming test, needs real hardware')
    config.addinivalue_line('markers', 'emu: split-pair test under ESP-EMU, no hardware or AWS')
    config.addinivalue_line(
        'markers',
        'manual: needs a person or an instrument CI does not have (a meter, a '
        'rewire, a phone app). Never runs in automation, even with --e2e.')


def pytest_collection_modifyitems(config, items):
    # --e2e needs a bench and AWS, --emu needs only esp-emu. Neither by default.
    skip_manual = pytest.mark.skip(reason='needs --manual (a person or an instrument)')
    want_manual = config.getoption('--manual')
    skip_e2e = pytest.mark.skip(reason='needs --e2e (hardware + Wi-Fi + AWS credentials)')
    skip_emu = pytest.mark.skip(reason='needs --emu (esp-emu; no hardware required)')
    want_e2e = config.getoption('--e2e')
    want_emu = config.getoption('--emu')
    for item in items:
        # manual wins: no flag combination makes these run unattended.
        if 'manual' in item.keywords and not want_manual:
            item.add_marker(skip_manual)
            continue
        if 'emu' in item.keywords:
            if not want_emu:
                item.add_marker(skip_emu)
        elif 'e2e' in item.keywords and not want_e2e:
            item.add_marker(skip_e2e)



def _creds_from_fragment():
    """The AWS keys in sdkconfig.creds, as boto3 session kwargs. {} if absent."""
    if not CREDS_FRAGMENT.exists():
        return {}
    found = dict(re.findall(r'^CONFIG_(AWS_\w+)="(.*)"$', CREDS_FRAGMENT.read_text(), re.M))
    key, secret = found.get('AWS_ACCESS_KEY_ID'), found.get('AWS_SECRET_ACCESS_KEY')
    if not (key and secret):
        return {}
    creds = {'aws_access_key_id': key, 'aws_secret_access_key': secret}
    if found.get('AWS_SESSION_TOKEN'):
        creds['aws_session_token'] = found['AWS_SESSION_TOKEN']
    return creds


@pytest.fixture(scope='session')
def aws_creds():
    """Explicit env credentials, or {} to let boto3 resolve its ambient chain."""
    key, secret = os.getenv('AWS_ACCESS_KEY_ID'), os.getenv('AWS_SECRET_ACCESS_KEY')
    if not (key and secret):
        # Reuse the device's own keys rather than failing on an empty environment.
        fragment = _creds_from_fragment()
        if fragment:
            return fragment
        # A profile / SSO / instance role is fine for the viewer side.
        return {}
    creds = {'aws_access_key_id': key, 'aws_secret_access_key': secret}
    if os.getenv('AWS_SESSION_TOKEN'):
        creds['aws_session_token'] = os.getenv('AWS_SESSION_TOKEN')
    return creds


@pytest.fixture(scope='session')
def wifi_creds():
    ssid, password = os.getenv('WIFI_SSID'), os.getenv('WIFI_PASSWORD')
    if not ssid:
        pytest.skip('set WIFI_SSID (and WIFI_PASSWORD) to run the e2e tests')
    return ssid, password or ''


@pytest.fixture(scope='session')
def kvs_channel(request):
    return request.config.getoption('--kvs-channel')


@pytest.fixture(scope='session')
def kvs_region(request):
    return request.config.getoption('--kvs-region')


@pytest.fixture(scope='session')
def creds_fragment(kvs_channel, kvs_region):
    """Path to the gitignored sdkconfig fragment carrying the device's AWS keys.

    A fragment the developer wrote by hand is left alone -- it may well point at IoT
    Core certs, which cannot be generated from env vars. Otherwise we synthesise the
    static-key flavour from the environment.
    """
    if CREDS_FRAGMENT.exists():
        _check_fragment(CREDS_FRAGMENT)
        return CREDS_FRAGMENT

    key, secret = os.getenv('AWS_ACCESS_KEY_ID'), os.getenv('AWS_SECRET_ACCESS_KEY')
    if not (key and secret):
        pytest.skip(f'no {CREDS_FRAGMENT.name}; set AWS_ACCESS_KEY_ID and '
                    f'AWS_SECRET_ACCESS_KEY so it can be generated')

    lines = [
        '# Generated by test_e2e/conftest.py -- gitignored, do not commit.',
        'CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n',
        f'CONFIG_AWS_ACCESS_KEY_ID="{key}"',
        f'CONFIG_AWS_SECRET_ACCESS_KEY="{secret}"',
        f'CONFIG_AWS_SESSION_TOKEN="{os.getenv("AWS_SESSION_TOKEN", "")}"',
        f'CONFIG_AWS_KVS_CHANNEL_NAME="{kvs_channel}"',
        f'CONFIG_AWS_DEFAULT_REGION="{kvs_region}"',
    ]
    CREDS_FRAGMENT.write_text('\n'.join(lines) + '\n')
    CREDS_FRAGMENT.chmod(0o600)
    return CREDS_FRAGMENT


def _check_fragment(path):
    """Fail loudly on a static-key fragment whose keys are blank.

    The generator deliberately leaves an existing fragment alone so a hand-written
    IoT-cert config survives. That makes a leftover placeholder easy to miss, and the
    symptom on device is an opaque signaling auth failure.
    """
    text = path.read_text()
    if 'CONFIG_IOT_CORE_ENABLE_CREDENTIALS=y' in text:
        return  # cert-based; keys are legitimately absent
    for symbol in ('CONFIG_AWS_ACCESS_KEY_ID', 'CONFIG_AWS_SECRET_ACCESS_KEY'):
        for line in text.splitlines():
            if line.startswith(symbol + '='):
                value = line.split('=', 1)[1].strip().strip('"')
                if not value or value.upper().startswith(('PLACEHOLDER', 'CHANGEME', 'XXX')):
                    pytest.fail(f'{path.name} has an empty or placeholder {symbol}. '
                                f'Delete the file to regenerate it from the environment.')
                break
        else:
            pytest.fail(f'{path.name} sets static credentials but never defines {symbol}')


# Function-scoped so it can read app_path, which is parametrised per test.
@pytest.fixture
def channel_matches_firmware(request, kvs_channel):
    """Fail early if the firmware was built for a different signaling channel.

    The viewer joins whatever --kvs-channel says while the device joins whatever was
    compiled into it, and nothing reports the mismatch: the offer simply goes to a
    channel the device is not on. The symptom is an opaque "device never answered the
    offer" after the full connect timeout, which is indistinguishable from a real
    failure -- so check it up front.

    Reads the config of the build that actually shipped rather than the fragment.
    """
    source, config = _built_config(request, _signaling_example(request))
    built = config.get('AWS_KVS_CHANNEL_NAME')
    if built and built != kvs_channel:
        pytest.fail(
            f'{_describe_build(source)} was built for channel {built!r} but the '
            f'viewer will join {kvs_channel!r}. They must match -- delete '
            f'test_e2e/sdkconfig.creds, re-run with the channel you want, and rebuild.')
    return built


def _signaling_example(request):
    """Name of the example whose build holds the KVS signaling connection.

    Usually the app under test. In split mode it is not: the test drives
    streaming_only on the P4, but the channel belongs to signaling_only on the
    co-processor, so follow the hop. Validating a split-mode run against any other
    project's config would check the wrong channel.
    """
    try:
        app_path = Path(request.getfixturevalue('app_path'))
    except Exception:
        return None
    return 'signaling_only' if app_path.name == 'streaming_only' else app_path.name


def _built_config(request, example=None):
    """(path, dict) of build/config/sdkconfig.json for the binary under test.

    Read from the build directory, not from examples/<app>/sdkconfig. That file is
    shared by every build dir of the example and describes only the last configure,
    so it can disagree with the binary actually flashed -- and a lookup through
    `<build>/..` lands on it too, which made --build-dir meaningless.

    The build dir is --build-dir (default `build`) under the app path. `example`
    selects a sibling example with the same build-dir name, which is how split mode
    reaches signaling_only's build from a streaming_only test; an absolute
    --build-dir is then taken by its last component. Returns (None, {}) when the
    config cannot be found or read.
    """
    import json
    try:
        app_path = Path(request.getfixturevalue('app_path'))
        build_dir = Path(request.getfixturevalue('build_dir') or 'build')
    except Exception:
        return None, {}
    if example and example != app_path.name:
        app_path = app_path.parent / example
        build_dir = Path(build_dir.name)
    if not build_dir.is_absolute():
        build_dir = app_path / build_dir
    source = build_dir / 'config' / 'sdkconfig.json'
    try:
        return source, json.loads(source.read_text())
    except (OSError, ValueError):
        return None, {}


def _describe_build(source):
    """`<example>/<build dir>` for a sdkconfig.json path, for log lines."""
    build_dir = source.parent.parent
    return f'{build_dir.parent.name}/{build_dir.name}'


# Function-scoped so it can read app_path, which is parametrised per test.
@pytest.fixture(autouse=True)
def report_capture_source(request):
    """Log whether the firmware captures from the camera or from sample files.

    The source is a compile-time #ifdef (CONFIG_MEDIA_STREAM_CAPTURE_FROM_FILE,
    webrtc_main.c), so it cannot be chosen from the command line -- it is decided by
    which overlay was in SDKCONFIG_DEFAULTS. Recording it next to the metrics keeps
    a camera run from being compared against a file run without anyone noticing:
    a live camera varies with scene and rate control, so its loss figures are not
    comparable with a fixed bitstream's.
    """
    log = logging.getLogger(__name__)
    source, config = _built_config(request)
    if source is None:
        log.info('capture source: unknown (no build config found; pass --build-dir)')
        return None
    from_file = bool(config.get('MEDIA_STREAM_CAPTURE_FROM_FILE'))
    capture = 'file (SPIFFS sample frames)' if from_file else 'camera'
    log.info('capture source: %s  [%s]', capture, _describe_build(source))
    return capture


@pytest.fixture(scope='session')
def aws_creds_usable(aws_creds, kvs_region):
    """Fail on expired or unusable credentials before anything is flashed.

    STS session keys are compiled into the firmware, so they expire mid-campaign and
    the device-side symptom is a signaling stall that reads like a network fault.
    One GetCallerIdentity turns that into a line naming the real problem -- and says
    that refreshing them means rebuild + reflash, not just a re-run.
    """
    boto3 = pytest.importorskip('boto3', reason='pip install -r test_e2e/requirements.txt')
    from botocore.exceptions import BotoCoreError, ClientError
    import contextlib as _ctx

    session = boto3.Session(region_name=kvs_region, **(aws_creds or {}))
    try:
        with _ctx.closing(session.client('sts')) as sts:
            return sts.get_caller_identity().get('Arn', 'unknown')
    except (BotoCoreError, ClientError) as exc:
        pytest.fail(f'AWS credentials are not usable: {exc}. If these are STS session '
                    f'keys they are baked into the firmware at build time, so refreshing '
                    f'them means rebuild + reflash, not just a re-run.')


# Where the co-processor's log comes from. With
# CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR the host re-prints it behind a [SLAVE]
# prefix, the only option on a one-port board. Without it the CP has its own UART.
# Reading the config rather than the bench keeps one test working on both, and lets
# the forwarding -- which otherwise puts a second writer on the host console.
# ---------------------------------------------------------------------------

def _as_pattern(pattern):
    if isinstance(pattern, str):
        return re.escape(pattern.encode())
    if isinstance(pattern, bytes):
        return pattern
    return pattern.pattern


class _ForwardedCp:
    """CP lines arrive inline on the host console, tagged by slave_flasher."""

    source = 'host console ([SLAVE] prefix)'
    uses_write_slave = True

    def __init__(self, dut):
        self.dut = dut

    def anchor(self, pattern):
        return re.compile(rb'\[SLAVE\].*?' + _as_pattern(pattern))

    def write(self, command):
        # Routed through the host's write_slave command; the caller owns retries.
        return None


class _DirectCp:
    """CP read straight from its own UART.

    Presents the small surface the helpers need -- expect() and logdir -- so the same
    assertions work against either source.
    """

    source = 'co-processor UART'
    uses_write_slave = False

    def __init__(self, port, logdir, baud=115200):
        import serial, threading
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self.logdir = str(logdir)
        self._logfile = Path(logdir) / 'dut.log'
        self._logfile.write_bytes(b'')
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = port, baud, 0.2
        # Deassert both straps BEFORE opening: on an esp-prog, DTR/RTS drive IO0/EN,
        # and opening with them asserted drops the chip into DOWNLOAD mode.
        # Deliberately no EN pulse here -- the co-processor is already running and
        # mid-session by the time this fixture is built, so resetting it would
        # destroy the very state the test is about to assert on.
        ser.dtr = False
        ser.rts = False
        ser.open()
        self._ser = ser
        self._thread = threading.Thread(target=self._drain, daemon=True)
        self._thread.start()

    def _drain(self):
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(4096)
            except Exception:
                return
            if chunk:
                with self._lock:
                    self._buf.extend(chunk)
                with open(self._logfile, 'ab') as fh:
                    fh.write(chunk)

    def expect(self, pattern, timeout=30):
        if not hasattr(pattern, 'search'):
            pattern = re.compile(_as_pattern(pattern))
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self._lock:
                m = pattern.search(bytes(self._buf))
            if m:
                return m
            time.sleep(0.05)
        raise TimeoutError(f'{pattern!r} not seen in {timeout}s on the co-processor console')

    def write(self, command):
        self._ser.write((command + '\r\n').encode())
        self._ser.flush()

    def close(self):
        self._stop.set()
        try:
            self._ser.close()
        except Exception:
            pass

    def anchor(self, pattern):
        # Nothing else shares this stream, so no prefix is needed to disambiguate.
        return re.compile(_as_pattern(pattern))

    @property
    def dut(self):
        # This object *is* the log source; _expect/_seen/_in_log take it directly.
        return self


@pytest.fixture
def cp_console(request, dut, tmp_path):
    """The co-processor's log, from whichever source the firmware implies."""
    _, cfg = _built_config(request)
    # Absent from sdkconfig.json when SLAVE_FLASHER_ENABLE is off (the symbol depends on
    # it): nothing re-prints the C6 log then, so ask for --cp-port rather than wait.
    forwarded = cfg.get('SLAVE_FLASHER_POST_FLASH_MONITOR', False)
    cp_port = request.config.getoption('--cp-port')

    if forwarded and not cp_port:
        src = _ForwardedCp(dut)
        logging.getLogger('conftest').info('co-processor log source: %s', src.source)
        yield src
        return

    if not cp_port:
        pytest.fail(
            'The host firmware is built without CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR, '
            'so it does not re-print the co-processor log. Pass --cp-port <port> so the '
            'test can read the co-processor directly.')

    src = _DirectCp(cp_port, tmp_path)
    logging.getLogger('conftest').info('co-processor log source: %s (%s)', src.source, cp_port)
    try:
        yield src
    finally:
        src.close()
