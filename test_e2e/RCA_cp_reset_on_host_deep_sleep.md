# RCA: CP resets when the host enters deep sleep

## Symptom
The C5 co-processor reboots each time the P4 host parks. The console shows:

```
I (27855) cp_light_sleep: Light sleep ENABLED
ESP-ROM:esp32c5-eco2-20250121
rst:0x1 (POWERON),boot:0x2c (DOWNLOAD(UART0/USB))
waiting for download
```

## Root cause
The host drives the two C5 strapping lines and releases them when it sleeps.

| Host pin | C5 pin | Owner | State in deep sleep |
| --- | --- | --- | --- |
| GPIO54 | EN (reset, active low) | `CONFIG_ESP_HOSTED_HOST_RESET_GPIO` | released |
| GPIO47 | GPIO0 (boot strap) | `CONFIG_SLAVE_GPIO0_TRIGGER_PIN` | released |

`CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND=1` makes IDF return the pads to their
default state on deep sleep entry. No code calls `gpio_hold_en()` on either pin.
`eh_host_port_power_save_enter()` calls `esp_deep_sleep_start()` directly
(`port/os/idf/src/eh_host_port_power.c:114`).

The released EN line resets the C5. The released GPIO0 line selects download mode.

## Evidence
1. The reset reason is `rst:0x1`, not a panic. The RTC canary is cleared.
2. The boot mode is `DOWNLOAD(UART0/USB)`. Only GPIO0 low can select it.
3. The reset always follows `Host Sleep`, never precedes it.
4. The fault occurs with PSRAM on and with PSRAM off. PSRAM is not a factor.

## Correction to an earlier finding
An earlier report named PSRAM plus light sleep as the cause. That report is wrong.
Those runs never entered light sleep, because the host never parked. The host
idle timer was not armed under network split. After the arming fix, both PSRAM
builds and non-PSRAM builds fail the same way.

## Fix
Hold both pads across deep sleep. The P4 supports this:
`SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1`, and the digital pad mask
`0x007FFFFFFFFF0000` covers GPIO16 to GPIO54.

## Second defect found: the board pin map depends on the slave flasher

`CONFIG_SLAVE_FLASHER_ENABLE` selects both the co-processor target and the board
pin map. `sdkconfig.defaults.esp32p4` keeps the correct lines commented out:

```
#CONFIG_SLAVE_IDF_TARGET_ESP32C5=y
#CONFIG_ESP_HOSTED_P4_C5_CORE_BOARD=y
CONFIG_ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD=y
```

If the slave flasher is off, the target falls back to ESP32-C6 and the SDIO pins
fall back to the development board map:

| Build | Board symbol | CLK | D0 |
| --- | --- | --- | --- |
| flasher on | `P4_C5_CORE_BOARD` | 50 | 49 |
| flasher off | `P4_DEV_BOARD_FUNC_BOARD` | 18 | 14 |

The bus then never enumerates. The host logs
`sdmmc_init_ocr: send_op_cond (1) returned 0x107`, and
`ESP_ERROR_CHECK(esp_hosted_connect_to_slave())` aborts at
`main/streaming_only_main.c:220`. The host reboots, and each host boot pulses the
co-processor EN line. That produces the co-processor reset loop.

Fix: set the target and the board explicitly in `sdkconfig.defaults.esp32p4`, so
that the board configuration does not depend on the slave flasher.

## Third defect: the slave flasher resets the co-processor on every host boot

`CONFIG_SLAVE_FLASHER_ENABLE=y` makes the host put the co-processor into
download mode at each host boot to check the co-processor firmware. A deep-sleep
wake is a host boot, so the co-processor is reset on every wake. The console
shows `boot:0x2c (DOWNLOAD(UART0/USB))`.

With the slave flasher off, and with the board pinned correctly, the
co-processor ran 673 s with 0 boots and 0 resets.

Correct set of options for the host:

```
CONFIG_SLAVE_FLASHER_ENABLE=n
CONFIG_SLAVE_IDF_TARGET_ESP32C5=y
CONFIG_ESP_HOSTED_P4_C5_CORE_BOARD=y
```

`CONFIG_SLAVE_IDF_TARGET_ESP32C5` is the symbol that selects the co-processor
target. The slave flasher sets it as a side effect. Set it directly.

## Fourth defect: CP_RESET_STRATEGY_ONLY_IF_NECESSARY runs out of memory

The strategy `ALWAYS` resets the co-processor on every host boot, so it defeats
co-processor light sleep. The strategy `ONLY_IF_NECESSARY` probes the SDIO card
first, but the probe runs before the PSRAM DMA pool is reserved:

```
I (1808) eh_sdio: transport[host]: SDIO 4-bit 40000 kHz ... D0=49
E (1834) sdmmc_common: sdmmc_allocate_aligned_buf: not enough mem, err=0x101
I (1851) esp_psram: Reserving pool of 32K of internal memory for DMA
I (2087) eh_sdio: Card init success, TRANSPORT_RX_ACTIVE
ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM)
```

The host then aborts and reboots. With `ALWAYS`, the reset pulse delays the card
initialisation past the pool reservation, so the fault is hidden.

## Fix and result

One configuration change gives a stable system. esp_hosted already handles the
wake.

### 1. Host configuration (`sdkconfig.defaults.esp32p4`)

```
CONFIG_SLAVE_FLASHER_ENABLE=n
CONFIG_SLAVE_IDF_TARGET_ESP32C5=y
CONFIG_ESP_HOSTED_P4_C5_CORE_BOARD=y
```

The slave flasher resets the co-processor at each host boot. The other two lines
keep the board pin map correct without the flasher.

### 2. esp_hosted: a power-save wake under the ALWAYS strategy

File: `host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c`,
function `ensure_slave_bus_ready()`.

The `ALWAYS` strategy resets the co-processor on every cold host boot. On a
power-save wake, esp_hosted skips the reset and only re-inits the
SDIO card. No change is needed there. The resets on the bench came from the
slave flasher (third defect).

The `ONLY_IF_NECESSARY` strategy is not a usable alternative. It probes on a cold
boot as well, where the probe runs before the PSRAM DMA pool exists and the host
aborts with `ESP_ERR_NO_MEM`.

### Measured result, 298 s window

| Metric | Before | After |
| --- | --- | --- |
| Co-processor boots | 18 to 46 | 1 (the initial boot) |
| Co-processor uptime | 31 s maximum | 292 s, no reset |
| Host parked | 0 s | 268 s of 298 s |
| Light sleep engaged | ended at each reset | held |

## Soak result

15 minutes, host build `bnf5`, co-processor build `bslimB` (no PSRAM):

```
host awake 0/894 s (0%), parked 894 s
CP boots 0, panics 0, resets NONE
CP uptime 353 s -> 1242 s continuous
```

The configuration reproduces from repository files alone:

```
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32p4;\
<your creds file>;sdkconfig.defaults.powersave" \
    idf.py -B build_ps -DSDKCONFIG=build_ps/sdkconfig build
```

## Open defect: the host never wakes

The co-processor asserts the wake line. The host does not wake.

Co-processor console at boot:

```
I (892) ehcp_host_ps: Host wakeup: IO3, level:1 (configure ret=0)
I (898) ehcp_host_ps: Host wakeup: IO3, level:0 (active HIGH)
I (904) ehcp_host_ps: Host wakeup: pulsed IO3 once at our boot
```

Host console before it parked:

```
I (8350) eh_ps: Armed wakeup GPIO 2 (active=1)
```

The co-processor drives C5 IO3. The host arms P4 GPIO2. The host stays parked.
`CONFIG_ESP_HOSTED_CP_FEAT_HOST_PS_WAKE_HOST_ON_CP_BOOT=y` is set, so a
co-processor reset must wake the host. It does not.

Note that `sdkconfig.defaults.esp32p4` asks for
`CONFIG_ESP_HOSTED_HOST_FEAT_POWER_SAVE_WAKEUP_GPIO=6`, but the board Kconfig
range for this board is 2 to 2, so the value becomes 2 without a warning.

Possible causes, in order of likelihood. None is confirmed:

1. C5 IO3 and P4 GPIO2 are not connected on this board.
2. The pulse is too short for the host deep-sleep GPIO wake. This is unlikely.
   `EH_CP_HOST_PS_BOOT_WAKE_PULSE_MS` is 1, the co-processor tick rate is
   1000 Hz, so `pdMS_TO_TICKS(1)` is one tick and the pulse is about 1 ms.
   P4 GPIO2 is a low-power GPIO, so it can latch a deep-sleep wake.
3. The host arms the wake source, but deep sleep entry clears it.

### Effect on the bench

The host parks and never returns. Its USB port stays off, so the host cannot be
reprogrammed. To recover the board, do one of these:

- Press the reset button on the ESP32-P4.
- Send a unicast packet to the host IP address from a device on the same Wi-Fi
  network. See `examples/power_save/.../host_wakeup_demo_using_test_packet.sh`
  in the esp_hosted component.

## Still unverified

1. PSRAM on the co-processor, under light sleep that actually engages. The
   earlier claim that PSRAM causes the resets is withdrawn. It was measured on
   runs where the host never parked.
2. Power figures. Measure them with valid AWS credentials. With expired ones the
   application retries `describeChannel`, so the figure is of a retry loop, not
   of a connected steady state.

## Wake path confirmed

A TCP SYN to host port 22, sent from another machine on the LAN, woke the host:

```
I (97366) ehcp_host_ps: Wakeup needed, reason sta tx msg
W (97370) Wakeup_pkt: ... 40 06 ... c0 a8 32 df c0 a8 32 ee f1 de 00 16 ...
I (1566)  streaming_only: reset reason: 8 (deep-sleep wake)
```

The co-processor was not reset, and its uptime ran unbroken from 97 s to 214 s.

## Open defect: the host re-initialises Wi-Fi on wake and drops the link

The host calls `esp_wifi_init()` again after a deep-sleep wake, with a config
that does not match the one the co-processor runs. The co-processor compares the
two and rebuilds the whole Wi-Fi stack.

Co-processor:

```
I (99552) ehcp_wifi_rpc: === __wrap_esp_wifi_init called ===
I (99557) ehcp_wifi_rpc: WiFi init config: static_rx_buf_num changed: 10 -> 16
I (99564) ehcp_wifi_rpc: WiFi init config: feature_caps changed: 3363 -> 3491
I (99572) ehcp_wifi_rpc: WiFi init config changed, reinitializing
I (99576) wifi:state: run -> init (0x0)
```

Host:

```
I (2378) eh_wifi: rx RPC StaDisconnected ssid="ESP_TEST_2G" reason=8
```

Reason 8 is ASSOC_LEAVE: the station left on its own. The association was healthy
at the time. The WebSocket then fails with `read error :-0x004C`, and the
signaling client needs 7.6 s to reconnect.

The comparison is in `wifi_init_config_changed()`
(`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c:164`).
It treats a change of buffer counts or feature bits as a reason to re-initialise,
even while a station is associated.

The same double call happens on a cold boot. The co-processor initialises with
16 buffers, and the host then sends 10, which forces one re-init. That is
harmless before association. After a wake it costs the link.

Two candidate fixes, neither implemented:

1. Host side: do not call `esp_wifi_init()` again on a power-save wake. The
   co-processor kept the station associated, so there is nothing to initialise.
2. Co-processor side: keep the link if only buffer sizing differs while a station
   is associated.

Fix 1 is the better one. The host must not rebuild state that it did not tear
down.

## Cosmetic: three SDIO timeouts on a cold boot

```
W (1556) eh_sdio: Reset co-processor using GPIO[54]
E (3104) sdmmc_common: sdmmc_init_ocr: send_op_cond (1) returned 0x107
```

`0x107` is `ESP_ERR_TIMEOUT`. The host resets the co-processor and then probes
the SDIO card before the co-processor has started its SDIO slave. The probe
succeeds about 400 ms later. A settle delay after the reset removes the noise.
This does not happen on the wake path, where the reset is skipped.
