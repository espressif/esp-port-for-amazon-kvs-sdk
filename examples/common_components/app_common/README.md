# App Common

Shared component used by all examples. Provides Wi-Fi initialization, SPIFFS/SD card storage setup, and common configuration options via `idf.py menuconfig`.

## Configuration Options

Available under `idf.py menuconfig`:

**App Common Configs (Wi-Fi, SD-Card, Camera)**:
- `ESP_WIFI_SSID` - Wi-Fi network name
- `ESP_WIFI_PASSWORD` - Wi-Fi password
- `ESP_MAXIMUM_RETRY` - Max Wi-Fi reconnection attempts
- `APP_COMMON_USE_SPIFFS` - Read certificates from SPIFFS partition
- SD card bus width and pin configuration

**BLE Provisioning** (under `App Common Configs`):
- `APP_NETWORK_PROV_BLE` - Enable BLE-based WiFi provisioning (requires `BT_ENABLED` and `BT_NIMBLE_ENABLED`)
- `APP_NETWORK_PROV_POP` - Proof of Possession string for provisioning security (default: `abcd1234`)

**AWS Security Credentials**:
- `AWS_KVS_CHANNEL_NAME` - KVS signaling channel name
- `AWS_DEFAULT_REGION` - AWS region
- `IOT_CORE_ENABLE_CREDENTIALS` - Use IoT Core credentials (default: enabled)
- IoT Core endpoint, certificate paths, role alias, and thing name
- Direct AWS credentials (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`) when IoT Core is disabled

## WiFi Provisioning

When `APP_NETWORK_PROV_BLE` is enabled, the device starts a BLE provisioning service on first boot (when no WiFi credentials are stored). Users can provide WiFi credentials using the **ESP BLE Provisioning** phone app ([Android](https://play.google.com/store/apps/details?id=com.espressif.provble) / [iOS](https://apps.apple.com/app/esp-ble-provisioning/id1473590141)).

### How It Works

1. On first boot, the device advertises as `PROV_XXXXXX` (derived from MAC address)
2. Open the ESP BLE Provisioning app, scan for the device, and enter the Proof of Possession string (default: `abcd1234`)
3. Select your WiFi network and enter the password
4. Credentials are stored in NVS — subsequent boots connect automatically

### Platform Support

| Platform | BLE Source | Notes |
|----------|-----------|-------|
| ESP32, ESP32-S3 | Native BLE | Direct NimBLE stack |
| ESP32-C6 | Native BLE | Direct NimBLE stack |
| ESP32-P4 | Coprocessor BLE | BLE runs on C6 via esp_hosted (`CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE`) |

On ESP32-P4, the BT controller on the C6 coprocessor is initialized before provisioning and released after completion.

### Resetting Provisioned Credentials

To re-provision, erase the NVS partition:
```bash
idf.py erase-flash
```

The `wifi-set <ssid> <password>` CLI command can also be used at runtime to change WiFi credentials without BLE. To clear stored credentials and re-trigger provisioning on next boot, use `wifi-set "" ""`.

## Contents

- `set_kvs_sdk_path.cmake` - CMake helper to locate the KVS SDK submodule
- `spiffs_image/` - SPIFFS partition contents (CA certificate, sample H.264 frames)
- `Kconfig.projbuild` - Menuconfig definitions for shared options
