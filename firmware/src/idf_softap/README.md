# `src/idf_softap/` — the ESP-IDF SoftAP firmware (T-Dongle-C5-idf)

This directory is a **second, self-contained firmware for the T-Dongle-C5**. It
is built only by the `T-Dongle-C5-idf` environment in `../platformio.ini`; the
Arduino environments (`T-Dongle-C5`, `-c2min`, `-selftest`, `-5g`, the LED
diags, and `T-Dongle-S3`) exclude it via `build_src_filter` and are unaffected
by anything in here.

## Why it exists

The Arduino C5 SoftAP is the known-bad path for issue #9 / open-questions #9: an
external client associates fine, the AP answers its first HTTP request, and then
the AP→STA downlink goes permanently dead below lwIP — the WiFi driver's TX
path accepts frames and reports success, yet nothing reaches the station. The
cause was isolated to the **arduino-esp32 WiFi wrapper plus the prebuilt Arduino
WiFi/PHY blobs**, not to sdkconfig and not to the application (see
[`issue-9-softap-findings.md`](../../../docs/knowledge-base/issue-9-softap-findings.md)).

The same board with a **from-source ESP-IDF SoftAP** and no Arduino WiFi at all
associates and serves HTTP indefinitely. This directory is that firmware, ported
in from the hybrid tree that was validated layer by layer on hardware.

## What's in here

| File | Role |
|---|---|
| `app_main.c` | `app_main()`: raw `esp_netif` + `esp_wifi` SoftAP (`Lily`, 192.168.4.1, ch6), the `c2_hooks_t` table wiring every subsystem to the HTTP layer, and the 1 Hz display/alive loop |
| `c2_server.c/.h` | `esp_http_server` route table (16 handlers) covering status, payloads, run/stop, recon and `GET /api/recon/pcap/<name>`; `lily_dashboard.h` is the HTML |
| `board_pins.h` | the C5 pin map this build needs (shared SPI2 bus, SD, LCD, APA102, AP channel) |
| `board_spi.c/.h` | sole owner of `SPI2_HOST`, a recursive bus mutex, and APA102 re-latch after bus traffic |
| `storage.c/.h` | SD over SPI, FATFS at `/sdcard`; payload listing/read/write plus the binary-safe PCAP read |
| `recon.c/.h`, `recon_config.h`, `pcap_writer.c/.h` | capture (pinned to the AP channel), scan, station enum, PCAP output |
| `st7735.c/.h`, `duck_display.c/.h` | pure-IDF ST7735 driver and the diff-gated 160×80 dashboard |
| `interpreter.cpp/.h`, `interp_hal.c/.h`, `duckcfg.h` | the DuckyScript VM, with a HID HAL that is an intentional no-op (the C5 has no USB-OTG) |
| `softap_secret.h` | pulls the SSID/password out of the existing gitignored `../config_secret.h` |

## Deliberate gaps vs the Arduino build

- **No serial console commands.** `SCAN`, `PMF`, `PMFDOWN`, `ENUM`, `DUMP`,
  `TXSTATS`, `LWIPSTATS` live in the Arduino `src/console/` module and are not
  part of this build. The control surface here is the REST API plus the LCD.
- **No AP-down PMF sweep.** `recon_start_pmf_sweep_ap_down()` returns `false`
  permanently — taking the SoftAP down needs a restore path that hung the
  reference firmware's main loop. The AP-up sweep and everything else are real.
- **HID is a no-op**, by silicon: the C5 has no USB-OTG peripheral.

## Building

```
cd firmware
pio run -e T-Dongle-C5-idf
```

Two config files at `firmware/` belong to this build and nothing else:

- `idf_softap_partitions.csv` — 2 MiB **factory**, no OTA (the image is ~1.11 MB)
- `idf_softap_sdkconfig.defaults` — the WiFi/LWIP/PSRAM keys, passed to CMake as
  `-DSDKCONFIG_DEFAULTS` because PlatformIO ignores `custom_sdkconfig` for
  `framework = espidf`. `CONFIG_SPIRAM` must stay **off** (issue #18).

Editing the sdkconfig defaults file does not automatically re-run CMake; delete
`sdkconfig.T-Dongle-C5-idf` or run `pio run -e T-Dongle-C5-idf -t clean` first.
