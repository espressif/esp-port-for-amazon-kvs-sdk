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
(packed from the consuming example's `target-firmware/` directory into a `slave`
SPIFFS partition) to the target. It's idempotent — a partition is re-flashed
only when its contents differ, so warm reboots skip it.

Build those three images from the co-processor firmware — `network_adapter` for
a plain network co-processor, or `signaling_only` for the split-mode signaling
C6 — and stage them per the `target-firmware/README.md` in the example.

## Configuration (`idf.py menuconfig` → *Slave Flasher Configuration*)

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_SLAVE_FLASHER_ENABLE`             | `y`   | Enable in-system flashing. Turn off on kits where the co-processor has its own USB port. |
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
