# Target (co-processor) firmware

On the ESP32-P4-EYE the P4 host runs this `esp_camera` example while the on-board
ESP32-C6 runs as a pure network co-processor. This directory holds the **C6
firmware images that the P4 flashes into the C6 in-system** (over UART), so you
don't need a separate cable/console on the C6.

## How it's used

`esp_camera`'s `CMakeLists.txt` packs this directory into a `slave` SPIFFS
partition (only when `CONFIG_SLAVE_FLASHER_ENABLE` is set — i.e. the P4-EYE
overlay):

```cmake
spiffs_create_partition_image(slave ${_slave_fw_dir} FLASH_IN_PROJECT)
```

At startup `app_main()` calls `flash_slave()` (from the `slave_flasher`
component), which reads these images out of that partition and programs the C6
at its standard offsets before Wi-Fi (which runs over the C6) is brought up.

## Files to place here

| File | What it is |
|------|------------|
| `bootloader.bin`      | C6 second-stage bootloader |
| `partition-table.bin` | C6 partition table |
| `app.bin`             | C6 application image |

## Where to build them from

The C6 runs the [`network_adapter`](../../network_adapter/README.md) example.
Build it for `esp32c6` with the P4-EYE pair overlay and copy the three outputs
here (rename the app image to `app.bin`):

```bash
cd ../../network_adapter
idf.py -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6;sdkconfig.defaults.p4_eye_pair.esp32c6' set-target esp32c6
idf.py build

DEST=../esp_camera/target-firmware
cp build/bootloader/bootloader.bin           "$DEST/bootloader.bin"
cp build/partition_table/partition-table.bin "$DEST/partition-table.bin"
cp build/network_adapter.bin                 "$DEST/app.bin"
```

Then rebuild `esp_camera` so the new images are packed into the `slave` partition.
