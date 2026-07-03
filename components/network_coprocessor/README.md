# Network Coprocessor Component

This is a **wrapper component** that provides the ESP-Hosted slave firmware (network coprocessor) functionality.

## Purpose

This component acts as a bridge between the application and the ESP-Hosted slave firmware embedded in the `esp_hosted` submodule. It allows using ESP chips (ESP32, ESP32-C5, ESP32-C6, etc.) as network coprocessors without duplicating the slave firmware source code.

## Architecture

```
network_coprocessor (wrapper component)
    └─> References files from: esp_hosted/slave/main/
    └─> Uses headers from: esp_hosted/common/
```

**No files are copied** - all source files are referenced directly from the `esp_hosted` submodule.

## Usage

This component is automatically included when building the `network_adapter` example:

```bash
cd examples/network_adapter
idf.py build
```

## Source Files Location

All actual source files are located in:
- `esp_hosted/slave/main/*.c` - Slave firmware implementation
- `esp_hosted/common/proto/*.proto` - RPC protocol definitions

## Configuration

Configuration options are sourced from the ESP-Hosted slave Kconfig.
Use `idf.py menuconfig` to configure transport (SDIO/SPI/UART) and other options.

## Supported Targets

- ESP32
- ESP32-C2
- ESP32-C3
- ESP32-C5
- ESP32-C6/C61
- ESP32-S2
- ESP32-S3

## Dependencies

- ESP-IDF >= 5.3
- `esp_hosted` component
- `esp_webrtc_utils` component
- `protocomm` (from ESP-IDF)

