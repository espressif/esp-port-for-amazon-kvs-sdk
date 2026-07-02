# KVS WebRTC unit tests

Unity-based unit tests for the platform-agnostic KVS WebRTC helper components.
They run on the **Linux host** (fast, no hardware) or on an **ESP32-S3**.

## What's covered

| Test file | Under test |
|-----------|------------|
| `test_aws_signer_v4.c`        | AWS SigV4 request signing |
| `test_base64.c`               | base64 encode/decode (`kvs_utils`) |
| `test_crc32.c`                | CRC32 (`kvs_utils`) |
| `test_hex.c`                  | hex encode/decode (`kvs_utils`) |
| `test_signaling_serializer.c` | `signaling_serializer` message (de)serialization |
| `test_state_machine.c`        | `state_machine` transitions |

## Run on the Linux host (fastest)

```bash
idf.py --preview set-target linux
idf.py build
./build/kvs_webrtc_tests.elf
```

At the menu, press ENTER to list the tests, then enter a test number, a test
name, or `*` to run them all.

## Run on ESP32-S3

```bash
idf.py set-target esp32s3
idf.py -p <PORT> flash monitor
```

Same interactive menu, over the serial console.

## Run under pytest (CI-style)

`pytest_kvs_webrtc.py` drives the menu automatically (sends `*`, asserts zero
failures) via `pytest-embedded`:

```bash
pytest pytest_kvs_webrtc.py --target linux      # or esp32s3
```

CI builds this app for `linux` and `esp32s3` — see `.build-test-rules.yml`.
