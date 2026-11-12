# Target (co-processor) firmware

On the ESP32-P4 dual-chip boards, webrtc_classic can flash the attached
ESP32-C6/C5 co-processor **in-system** — the P4 host writes the co-processor
image (packed here into a `slave` SPIFFS partition) over the UART link, so you
don't need to attach a separate cable/console to the C6/C5.

This only applies to the P4 board overlays that enable it (`p4_eye`, `p4x_eye`,
`p4_c5_core_board`). The single-chip ESP32/ESP32-S3 builds and the P4 kits with
a separate C6 USB port (`p4_function_ev_board_*`) don't use it — flash the
co-processor directly there.

## How it's used

When `CONFIG_SLAVE_FLASHER_ENABLE=y`, the `slave_flasher` component packs this
directory into a `slave` SPIFFS partition. At startup the app calls
`flash_slave()`, which programs the C6/C5. The build fails if a file below is
missing or empty, so you can't accidentally flash an empty partition.
To embed the images in the app instead, set
`CONFIG_SLAVE_FLASHER_IMAGE_EMBEDDED=y` (see `components/slave_flasher/README.md`).

## Files to place here

| File | What it is |
|------|------------|
| `bootloader.bin`      | C6/C5 second-stage bootloader |
| `partition-table.bin` | C6/C5 partition table |
| `app.bin`             | C6/C5 application image |

These are **not** checked into the repo.

## Where to build them from

The co-processor runs the [`network_adapter`](../../network_adapter/README.md)
example (Wi-Fi passthrough over SDIO). Build it for the co-processor target and
copy the outputs here (rename the app image to `app.bin`):

```bash
cd ../../network_adapter
idf.py set-target esp32c6          # or esp32c5 for p4_c5_core_board

DEST=../webrtc_classic/target-firmware
cp build/bootloader/bootloader.bin            "$DEST/bootloader.bin"
cp build/partition_table/partition-table.bin  "$DEST/partition-table.bin"
cp build/network_adapter.bin                  "$DEST/app.bin"
```

Then rebuild `webrtc_classic` for the P4 board so the images are packed into the
`slave` partition.
