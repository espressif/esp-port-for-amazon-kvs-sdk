# Network Adapter

This example provides Wi-Fi network connectivity from a co-processor (ESP32-C6 or ESP32-C5) to the main processor (ESP32-P4) on dual-chip boards. It uses the [esp_hosted](https://github.com/espressif/esp-hosted) component to transparently forward network traffic between the two chips.

## When to Use

Use this example when running `webrtc_classic` on ESP32-P4 in a dual-chip setup. The ESP32-P4 does not have built-in Wi-Fi, so the co-processor handles all Wi-Fi connectivity and forwards packets to the P4 over SDIO.

It's equally useful for any standalone ESP32-P4 application that relies on `esp_hosted` for network connectivity — for example, ESP RainMaker's [standalone camera example](https://github.com/espressif/esp-rainmaker/tree/master/examples/camera/standalone).

This is different from `signaling_only`, which runs signaling logic on the co-processor. Here, the co-processor acts purely as a network adapter with no application logic.

## Hardware Requirements

Pick one of the supported host + co-processor pairings:

| Host board | Co-processor target | Overlay |
|---|---|---|
| ESP32-P4 Function EV Board | ESP32-C6 (on-board) | `sdkconfig.defaults.esp32c6` (default) |
| ESP32-P4-EYE | ESP32-C6 (socketed on the EYE PCB) | `sdkconfig.defaults.esp32c6` + `sdkconfig.defaults.p4_eye_pair.esp32c6` |
| ESP32-P4 + external C5 module | ESP32-C5 | `sdkconfig.defaults.esp32c5` |

Flashing the co-processor needs ESP-Prog (or another USB-UART adapter) on the C6's `J2`/Prog-C6 header — the C6 has no on-board UART bridge. Wiring:

| ESP32-C6 (J2 / Prog-C6) | ESP-Prog |
|---|---|
| IO0 | IO9 |
| TX0 | TXD0 |
| RX0 | RXD0 |
| EN | EN |
| GND | GND |

## Setup

### Step 1 — Pick the build chain for your board

**ESP32-P4 Function EV Board pair (default C6 build):**
```bash
cd examples/network_adapter
idf.py set-target esp32c6
idf.py build
```

**ESP32-P4-EYE pair (P4-EYE PCB has a non-default SDIO slave pinout):**
```bash
cd examples/network_adapter
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.p4_eye_pair.esp32c6' set-target esp32c6
idf.py build
```
The `p4_eye_pair` overlay must be passed explicitly via `-D SDKCONFIG_DEFAULTS=…` — IDF auto-picks the base and per-target (`sdkconfig.defaults.<target>`) defaults on `set-target`, but not pair overlays. Pass the chain in the **same** `set-target` invocation, with `-D` **before** `set-target` — a bare `set-target esp32c6` followed by a separate `reconfigure` bakes the base config first and the overlay then loses to it (the C6 comes up with `CONFIG_ESP32P4_EYE_C6_BOARD=n`). It flips `CONFIG_ESP32P4_EYE_C6_BOARD=y`, which wires the SDIO slave pins for the P4-EYE socket; without it the C6 will not enumerate over SDIO on the P4-EYE PCB.

**External C5 module:**
```bash
cd examples/network_adapter
idf.py set-target esp32c5
idf.py build
```

### Step 2 — Flash the co-processor

```bash
# Flash via ESP-Prog (usually the second USB serial port)
idf.py -p /dev/ttyUSB1 flash monitor
```

### Step 3 — Flash the host

Build and flash your P4-side example (e.g. `webrtc_classic`, `streaming_only`, or a RainMaker camera example) on the ESP32-P4. The P4 brings up Wi-Fi over SDIO via this co-processor firmware.

> **Tip:** If your P4-side example uses the `slave_flasher` component, the C6 image is staged in `target-firmware/` on the host and reflashed automatically on first boot. In that case you do not need to manually flash the C6 — build it once, drop the `bootloader.bin`, `partition-table.bin`, and `app.bin` into the host project's `target-firmware/` directory, and let the host flash it over the SDIO/UART bridge.

## Troubleshooting

- **No IP address on P4**: Ensure the network_adapter firmware is running on C6 before booting P4. Check SDIO connection between the chips.
- **Build errors**: Run `idf.py set-target esp32c6` if the target is not already configured.
- **Cannot flash C6**: Verify ESP-Prog pin connections and that the correct serial port is used.

## Related Examples

- [webrtc_classic](../webrtc_classic/) - Main WebRTC example that uses this network adapter on P4
- [signaling_only](../signaling_only/) - Alternative co-processor firmware that handles signaling logic (for split mode)
