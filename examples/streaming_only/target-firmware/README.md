# Target (co-processor) firmware

In split mode the ESP32-P4 host runs this `streaming_only` example while an
ESP32-C6 co-processor runs the signaling side. This directory holds the **C6
firmware images that the P4 flashes into the C6 in-system** — so you don't
need to attach a separate cable/console to the C6.

## How it's used

`streaming_only`'s `CMakeLists.txt` packs this directory into a `slave` SPIFFS
partition:

```cmake
spiffs_create_partition_image(slave ${CMAKE_CURRENT_SOURCE_DIR}/target-firmware FLASH_IN_PROJECT)
```

At startup the app calls `flash_slave()` (from the `slave_flasher` component),
which reads the images below out of that partition and programs the C6 over the
UART link at their standard offsets. If the images are missing the P4 boots
normally but cannot (re)flash the C6.

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
