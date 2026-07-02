# Building & Flashing

The one canonical build/flash guide for every example in this repository. Each
example README links here for the generic steps and keeps only its example-specific
notes.

## Prerequisites

### Hardware
- An ESP32-series board. Most examples target **ESP32-P4** (with an **ESP32-C6** as the
  Wi-Fi co-processor on P4+C6 boards), **ESP32-S3**, or **ESP32**.
- A USB cable; for the C6 co-processor on boards without an onboard UART, an **ESP-Prog**
  (or other JTAG/UART adapter) — see [Dual-chip setup](#dual-chip-setup-esp32-p4--esp32-c6).

### Software
- **ESP-IDF v5.4 or v5.5** (`release/v5.4` or `release/v5.5`).
- On Linux, `pkg-config`.

## Clone

Clone with submodules (the upstream WebRTC SDK is a submodule):

```bash
git clone --recursive <git url>
```

If you already cloned without `--recursive`:

```bash
cd <cloned-dir>
git submodule update --init
```

## SDK patches (auto-applied)

The upstream submodule needs a few platform patches on top of its recorded SHA. CMake
applies them at configure time via `patches/apply_patches.sh` — a fresh clone plus
`idf.py build` is enough. The script is idempotent (a no-op if already applied).

To manage the submodule yourself (e.g. rebasing onto a different SDK tip), disable
auto-apply in menuconfig (`KVS WebRTC Configuration → Auto-apply webrtc-c platform
patches`) and apply manually:

```bash
cd amazon-kinesis-video-streams-webrtc-sdk-c
git am ../patches/*.patch
cd ..
```

What each patch does (and which upstream PR lets us drop it) is in
[`patches/README.md`](../patches/README.md).

## Install ESP-IDF

Follow the [Espressif getting-started guide](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html):

```bash
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git esp-idf
export IDF_PATH=<path-to-esp-idf>
cd $IDF_PATH && ./install.sh && . ./export.sh
```

`KVS_SDK_PATH` (the path to this repo) is resolved automatically by
`examples/common_components/set_kvs_sdk_path.cmake` (it defaults to the repo root). Set it
explicitly only if you build an example from outside the tree.

## Build an example

How you build depends on whether your board is **single-chip** or **dual-chip (ESP32-P4 + C6)** —
this is the first thing to get right.

### Single-chip (ESP32, ESP32-S3)

Everything (Wi-Fi + WebRTC) runs on one SoC. Plain `set-target`, configure, build:

```bash
cd examples/webrtc_classic
idf.py set-target esp32s3      # or esp32
idf.py menuconfig              # Wi-Fi, AWS, board options
idf.py build flash monitor
```

Both dual-chip flows flash the **C6 first**, then build the P4 app **with the board-overlay
chain** — a bare `set-target esp32p4` is not enough (see
[Board overlays](#board-overlays-sdkconfig_defaults-chain)). They differ in *what* runs on each
chip. C6 flashing/wiring details are in [Dual-chip setup](#dual-chip-setup-esp32-p4--esp32-c6).

### Dual-chip — classic (C6 as network adapter)

The P4 runs the entire WebRTC stack; the C6 is just a transparent Wi-Fi NIC running
`network_adapter`.

```bash
# 1) C6 — network adapter:
cd examples/network_adapter
idf.py set-target esp32c6
idf.py -p <C6_PORT> flash

# 2) P4 — the classic app, overlay chain in the SAME set-target call:
cd ../webrtc_classic
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py menuconfig              # Wi-Fi, AWS
idf.py build flash monitor
```

### Dual-chip — split mode (C6 signaling + P4 streaming)

Two cooperating firmwares: the C6 runs `signaling_only` (holds the KVS signaling channel), the P4
runs `streaming_only` (media). See [Deployment Modes → Split Mode](../README.md#deployment-modes).

```bash
# 1) C6 — signaling:
cd examples/signaling_only
idf.py set-target esp32c6
idf.py -p <C6_PORT> flash monitor   # set Wi-Fi + AWS creds in menuconfig first

# 2) P4 — streaming, overlay chain in the SAME set-target call:
cd ../streaming_only
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py build flash monitor
```

## Board overlays (SDKCONFIG_DEFAULTS chain)

Board-specific configuration lives in overlay files named
`sdkconfig.defaults.<board>[.<target>]` (e.g. `sdkconfig.defaults.p4_eye.esp32p4`,
`sdkconfig.defaults.p4_function_ev_board_v12.esp32p4`). Select a board by passing the
overlay chain via `SDKCONFIG_DEFAULTS`:

```bash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32p4;sdkconfig.defaults.p4_eye.esp32p4" \
       set-target esp32p4
idf.py build
```

> **Gotcha — pass the chain in the SAME invocation as `set-target`.** `set-target`
> regenerates `sdkconfig` from `SDKCONFIG_DEFAULTS`. If you run a bare `set-target`
> first and add the chain later, the already-generated `sdkconfig` wins for *choice*
> symbols (board/BSP selection) and your overlay is silently ignored. Always combine
> them, or delete `sdkconfig` and `reconfigure` with the chain.

## ws-on vs ws-off (signaling transport)

The KVS signaling path can run two ways:

- **ws-on (default)** — `esp_websocket_client`, lighter on stack/memory.
- **ws-off** — the upstream `libwebsockets` path (registry `espressif/libwebsockets`).
  Enable it in `idf.py menuconfig` (no overlay file) by setting three options
  together: `CONFIG_USE_ESP_WEBSOCKET_CLIENT=n`, `CONFIG_APP_WEBRTC_TASK_STACK_SIZE=32768`
  (the lws create path is stack-hungry), and `CONFIG_LWS_WITH_THREADPOOL=y` (the lws
  worker threadpool). See
  [kvs_signaling](../components/signaling/kvs_signaling/README.md#signaling-transport-ws-on--ws-off).

## Console output

Set the console channel in menuconfig (`Component config → ESP System Settings →
Channel for console output`):

- **USB Serial/JTAG** — ESP32-P4 Function EV Board v1.2 / v1.5, and the P4-EYE (native USB-C).
- **UART0 (default)** — ESP32-P4 Function EV Board v1.4.

If you only see bootloader logs, the console channel is wrong — fix it, rebuild, reflash.

## Dual-chip setup (ESP32-P4 + ESP32-C6)

On a P4+C6 board, flash [`examples/network_adapter`](../examples/network_adapter/) on the
C6 first. Boards without an onboard C6 UART need an ESP-Prog (or similar):

| ESP32-C6 (J2/Prog-C6) | ESP-Prog |
|-----------------------|----------|
| IO0 | IO9 |
| TX0 | TXD0 |
| RX0 | RXD0 |
| EN  | EN   |
| GND | GND  |

In `network_adapter`'s menuconfig (**Example Configuration**) set `ESP_WIFI_SSID`,
`ESP_WIFI_PASSWORD`, `AWS_KVS_CHANNEL_NAME`, `AWS_DEFAULT_REGION`.

**Single-USB-port kits (e.g. P4-EYE):** enable `CONFIG_SLAVE_FLASHER_ENABLE` so the P4
flashes the C6 over the internal UART (no separate C6 port / ESP-Prog needed).

## AWS credentials

Credentials go through the shared `credential` component (`aws_credentials_get()`):

- **IoT Core (default)** — `IOT_CORE_ENABLE_CREDENTIALS` on. Configure under **AWS
  Security Credentials**; place certs in
  [`examples/common_components/app_common/spiffs_image/certs`](../examples/common_components/app_common/spiffs_image/certs/).
- **Static access keys** — disable `IOT_CORE_ENABLE_CREDENTIALS`, set `AWS_ACCESS_KEY_ID`
  and `AWS_SECRET_ACCESS_KEY`.

## Wi-Fi / BLE provisioning

Network provisioning (including BLE Wi-Fi provisioning) is documented once in
[`examples/common_components/app_common/README.md`](../examples/common_components/app_common/README.md) — examples reuse it
rather than repeating it.

## Troubleshooting

- **Partition table doesn't fit flash.** The configured flash size must accommodate the
  whole partition table. A 16 MB partition layout (e.g. a 7 MB `ota_0` + spiffs/fctry/
  storage extending past ~10 MB) will not fit an 8 MB `CONFIG_ESPTOOLPY_FLASHSIZE` — set
  the flash size to match the board (16 MB) in the board overlay. Conversely a small
  (~3 MB) single-`factory` layout fits 8 MB fine.
- **Partition table offset.** Examples use `CONFIG_PARTITION_TABLE_OFFSET=0x9000` (some
  layouts use `0xC000`); the first app partition must be 64 KB-aligned (`0x10000`/`0x20000`).
- **Only bootloader logs on the monitor.** Wrong console channel — see
  [Console output](#console-output).
- **Build/Wi-Fi/allocation failures on P4+C6.** Confirm the C6 `network_adapter` is
  flashed and the SDIO/UART transport is wired per [Dual-chip setup](#dual-chip-setup-esp32-p4--esp32-c6).
