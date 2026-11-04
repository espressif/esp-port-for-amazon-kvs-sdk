# slave_flasher

Lets an ESP32-P4 host flash its attached co-processor (ESP32-C6/C5)
**in-system** over UART — no separate cable to the co-processor needed. It's a
**component**, not a standalone example: the P4 examples (`streaming_only`,
`webrtc_classic`, and the RainMaker standalone camera) depend on it and call it
at startup.

## Usage

```c
#include "slave_flasher.h"

flash_slave();   // program the attached co-processor
```

`flash_slave()` writes `bootloader.bin`, `partition-table.bin` and `app.bin`
to the target. It's idempotent — a partition is re-flashed only when its
contents differ, so warm reboots skip it.

## Image source

The component reads the three images from `CONFIG_SLAVE_FLASHER_IMAGE_DIR`
(default `target-firmware`, relative to the project directory). The build fails
if a file is missing or empty. `CONFIG_SLAVE_FLASHER_IMAGE_SOURCE` selects where
the images go:

- **SPIFFS partition** (default): the component packs the images into the
  `slave` SPIFFS partition, and `idf.py flash` writes it. The partition table
  must have a `slave` partition. An app OTA does not update this partition, so
  the co-processor keeps its factory image.
- **Embedded in the app**: the component embeds the images in the app binary.
  Each app OTA brings its own slave image. The project needs no `slave`
  partition.

The embedded images add about 1.4 MB to the app, so size the `ota_*` slots
to match. `flash_slave()` reads them in place from flash-mapped rodata. With
`CONFIG_SPIRAM_XIP_FROM_PSRAM=y`, IDF copies all rodata, and so the images,
to PSRAM at boot.

Build those three images from the co-processor firmware — `network_adapter` for
a plain network co-processor, or `signaling_only` for the split-mode signaling
C6 — and stage them per the `target-firmware/README.md` in the example.

## Configuration (`idf.py menuconfig` → *Slave Flasher Configuration*)

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_SLAVE_FLASHER_ENABLE`             | `y`   | Enable in-system flashing. Turn off on kits where the co-processor has its own USB port. |
| `CONFIG_SLAVE_FLASHER_IMAGE_SOURCE`       | SPIFFS | Image source: `slave` SPIFFS partition or embedded in the app |
| `CONFIG_SLAVE_FLASHER_IMAGE_DIR`          | `target-firmware` | Image directory for both sources |
| `CONFIG_SLAVE_UART_RX_PIN` / `_TX_PIN`    | 22 / 21 | Host UART pins to the target |
| `CONFIG_SLAVE_GPIO0_TRIGGER_PIN`          | 20    | Drives the target BOOT pin into download mode |
| `CONFIG_SLAVE_FLASHER_POST_FLASH_MONITOR` | `n`   | Pipe the target's UART output to the host log after flashing (debug only) |

### Per-board pins

The board overlays already set these for the supported P4 boards — for reference:

| Board | RX (`_UART_RX_PIN`) | TX (`_UART_TX_PIN`) | BOOT (`_GPIO0_TRIGGER_PIN`) |
| :---- | :-----------------: | :-----------------: | :-------------------------: |
| ESP32-P4 Function EV Board (default) | 22 | 21 | 20 |
| ESP32-P4-EYE / P4X-EYE               | 36 | 35 | 33 |
| ESP32-P4 + ESP32-C5 core board       | 45 | 46 | 47 |

On a custom wiring, set the three pins to match your host↔co-processor connections.
