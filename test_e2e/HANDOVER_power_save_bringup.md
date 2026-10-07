# Bring up host deep sleep and co-processor light sleep on a second bench

This procedure builds, flashes and checks both sides of the ESP32-P4 + ESP32-C5
core board. Follow it in order. Each step says what to expect.

## 1. What you need

| Item | Value |
| --- | --- |
| Board | ESP32-P4 + ESP32-C5 core board |
| ESP-IDF | master, `v6.2-dev-2630-gbdc0746c657` or newer |
| esp_hosted | 3.0.10, pulled by the component manager |
| Host application | `examples/streaming_only`, runs on the P4 |
| Co-processor application | `examples/signaling_only`, runs on the C5 |
| AWS | a Kinesis Video Streams signaling channel and temporary credentials |
| Wi-Fi | a 2.4 GHz access point |

The C5 has its own USB-UART bridge. Program it directly. The host does not need
to program it, and the slave flasher is off for that reason.

Do not use ESP-IDF 5.5 for these build directories. The two versions resolve the
configuration differently. Mixing them silently turns the slave flasher back on.

## 2. Environment

```
cd <repo>
. ~/esp-idf/export.sh
idf.py --version          # expect v6.2-dev
git checkout feat/host-deep-sleep-cp-light-sleep
git submodule update --init --recursive
./patches/apply_patches.sh
```

## 3. Credentials

Write `test_e2e/sdkconfig.creds`. The file is gitignored. Do not commit it.

```
CONFIG_IOT_CORE_ENABLE_CREDENTIALS=n
CONFIG_AWS_ACCESS_KEY_ID="ASIA..."
CONFIG_AWS_SECRET_ACCESS_KEY="..."
CONFIG_AWS_SESSION_TOKEN="..."
```

Only the co-processor talks to AWS. The host needs no credentials.

Set the Wi-Fi credentials at run time, from the co-processor console:

```
wifi-set <ssid> <password>
```

## 4. Build the co-processor, on the C5

```
cd examples/signaling_only
rm -rf cp

export SD_CP="sdkconfig.defaults;sdkconfig.defaults.esp32c5;\
sdkconfig.defaults.p4_c5_core_board.esp32c5;$PWD/../../test_e2e/sdkconfig.creds"

SDKCONFIG_DEFAULTS="$SD_CP" idf.py -B cp -DSDKCONFIG=cp/sdkconfig set-target esp32c5
```

Apply the esp_hosted patches now. `set-target` is what creates
`managed_components/`, and the component manager overwrites that directory, so
the patches must go on after it:

```
cd ../.. && ./patches/esp_hosted/apply.sh && cd examples/signaling_only
SDKCONFIG_DEFAULTS="$SD_CP" idf.py -B cp -DSDKCONFIG=cp/sdkconfig build
```

`rm -rf cp` is required. `set-target` does not re-read the defaults files when a
`sdkconfig` already exists in the build directory.

### For a power measurement, add the low-power overlay

```
export SD_CP="$SD_CP;sdkconfig.defaults.lowpower"
```

That file pins the Wi-Fi buffer counts, the softAP switch and the MSPI pad state
to the values from the build that measured lowest. Without it the numbers are
not comparable.

## 5. Build the host, on the P4

```
cd ../streaming_only
rm -rf host

export SD_HOST="sdkconfig.defaults;sdkconfig.defaults.esp32p4;\
sdkconfig.defaults.p4_c5_core_board.esp32p4;$PWD/../../test_e2e/sdkconfig.creds;\
sdkconfig.defaults.powersave"

SDKCONFIG_DEFAULTS="$SD_HOST" idf.py -B host -DSDKCONFIG=host/sdkconfig set-target esp32p4
cd ../.. && ./patches/esp_hosted/apply.sh && cd examples/streaming_only
SDKCONFIG_DEFAULTS="$SD_HOST" idf.py -B host -DSDKCONFIG=host/sdkconfig build
```

## 6. Check the configuration before you flash

```
grep -c SLAVE_FLASHER_ENABLE host/config/sdkconfig.h            # expect 0
grep -E "P4_C5_CORE_BOARD|SDIO_D0_GPIO_RANGE_MIN|WAKEUP_GPIO " host/config/sdkconfig.h
grep -E "^CONFIG_WIFI_RMT_(STATIC_RX_BUFFER_NUM|ENTERPRISE_SUPPORT)" host/sdkconfig
strings host/streaming_only.bin | grep -c "Host woke up from power save"   # expect 1
strings ../signaling_only/cp/signaling_only.bin | grep -c "Wakeup not needed, reason"  # expect 1
```

Expect `ESP_HOSTED_P4_C5_CORE_BOARD 1`, `SDIO_D0_GPIO_RANGE_MIN 49`,
`WAKEUP_GPIO 2`, `WIFI_RMT_STATIC_RX_BUFFER_NUM=10` and
`WIFI_RMT_ENTERPRISE_SUPPORT=n`.

A `SDIO_D0` of 14 means the board file did not apply. Delete the build directory
and configure again.

## 7. Find the ports

```
ls /dev/cu.usbserial*      # the C5, always present
ls /dev/cu.usbmodem*       # the P4, present only while it is awake
```

The P4 port disappears when the host parks. That is correct behaviour.

## 8. Flash the co-processor

Close any console on the C5 port first.

```
cd examples/signaling_only/cp
python -m esptool --chip esp32c5 -p <C5 port> -b 460800 write_flash \
  0x2000   bootloader/bootloader.bin \
  0xc000   partition_table/partition-table.bin \
  0x20000  signaling_only.bin \
  0x360000 storage.bin
```

## 9. Flash the host

Press `RESET` on the P4 so the port appears. Then hold `BOOT`, tap `RESET`,
release `BOOT`. The P4 stays in download mode and the port stays up.

```
cd ../../streaming_only/host
python -m esptool --chip esp32p4 -p <P4 port> -b 460800 write_flash \
  0x2000   bootloader/bootloader.bin \
  0x9000   partition_table/partition-table.bin \
  0x20000  streaming_only.bin \
  0x300000 storage.bin
```

Do not write `0x400000 slave.bin`. That partition only fed the slave flasher.

## 10. Watch both consoles

```
idf.py -B cp   -p <C5 port> monitor      # stays up
idf.py -B host -p <P4 port> monitor      # ends when the host parks
```

Set the Wi-Fi credentials on the co-processor console if the board is new:

```
wifi-set <ssid> <password>
```

## 11. Pass criteria

The host arms a 20 second idle timer once it holds an address. Watch the
co-processor console for about 5 minutes.

| Expect | Meaning |
| --- | --- |
| `Got IP: 192.168.x.x` | the link is up |
| no `HTTP request failed with status code 403` | the credentials work |
| `Signaling client connected` | the signaling walk finished |
| `Host Sleep` | the host asked to park |
| `Light sleep ENABLED` | the co-processor entered light sleep |
| the P4 port disappears | the host parked |
| **no further `ESP-ROM:esp32c5` banner** | **the fix holds** |
| co-processor uptime keeps rising past 60 s | no resets |

A failure looks like a co-processor boot banner every 30 seconds.

## 12. Test the wake

Send a packet to the host from a machine on the same Wi-Fi network. The host
owns TCP ports 22 and 8554, so a connection attempt to port 22 wakes it.

```
nc -z -w 2 <device ip> 22
```

The device address is printed as `Got IP` on the co-processor console. The host
and the co-processor share one address, and the co-processor decides per packet
which side owns it.

| Co-processor log | Meaning |
| --- | --- |
| `Wakeup needed, reason sta tx msg` | the packet is for the host |
| `Host woke up from power save` on the host | the wake path works |
| `Wakeup not needed, reason <rule>` | the co-processor kept the packet, and says why |

`Wakeup not needed, reason sta non-unicast` is normal for broadcast and
multicast traffic. It means ordinary network noise does not wake the host.

If nothing is logged at all, the packet never arrived. Check that the sending
machine is on the same network as the board.

## 13. Recovery

The host has no timer wake. Once it parks it returns only on a packet, or on a
`RESET` press. Its USB-serial-JTAG port powers down in deep sleep, so a parked
host cannot be reprogrammed. Press `RESET` on the P4 to get the port back.

## 14. Known limits

1. The automated power-save tests run under the emulator only. They cover the
   idle timer, the wake and the timer re-arm. They do not cover the C5 light
   sleep, the wake line, or the co-processor reset path.
2. No test asserts current. Power is measured by hand.
3. `patches/esp_hosted/apply.sh` must run again after any command that
   re-resolves components. `managed_components/` is not in git.
