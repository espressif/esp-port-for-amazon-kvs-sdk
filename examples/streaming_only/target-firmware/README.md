# Target (co-processor) firmware

In split mode the ESP32-P4 host runs this `streaming_only` example while an
ESP32-C6 co-processor runs the signaling side. This directory holds the **C6
firmware images that the P4 flashes into the C6 in-system** — so you don't
need to attach a separate cable/console to the C6.

## How it's used

When `CONFIG_SLAVE_FLASHER_ENABLE=y`, the `slave_flasher` component packs this
directory into a `slave` SPIFFS partition. The build fails if a file below is
missing or empty.

At startup the app calls `flash_slave()`, which reads the images below out of
that partition and programs the C6 over the UART link at their standard
offsets. To embed the images in the app instead, set
`CONFIG_SLAVE_FLASHER_IMAGE_EMBEDDED=y` (see `components/slave_flasher/README.md`).

## Files to place here

| File | What it is |
|------|------------|
| `bootloader.bin`      | C6 second-stage bootloader |
| `partition-table.bin` | C6 partition table |
| `app.bin`             | C6 application image |

## Where to build them from

The C6 runs the [`signaling_only`](../../signaling_only/README.md) example.
Build it for `esp32c6` and copy the three outputs here (the app image is named
after the project, so rename it to `app.bin`):

```bash
cd ../../signaling_only
idf.py set-target esp32c6
idf.py build

DEST=../streaming_only/target-firmware
cp build/bootloader/bootloader.bin            "$DEST/bootloader.bin"
cp build/partition_table/partition-table.bin  "$DEST/partition-table.bin"
cp build/signaling_only.bin                   "$DEST/app.bin"
```

Then rebuild `streaming_only` so the new images are packed into the `slave`
partition. You can also drop in your own C6 images if you maintain a custom
co-processor firmware — just keep the three file names above.
