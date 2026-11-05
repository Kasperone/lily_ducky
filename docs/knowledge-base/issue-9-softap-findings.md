# Issue #9 — SoftAP AP->STA TX stall (T-Dongle-C5 / ESP32-C5) — findings
_Date: 2026-10-30_

## Problem
On T-Dongle-C5 (ESP32-C5), lily_ducky's SoftAP stops sending AP->STA traffic below lwIP: clients can't connect or get no HTTP replies, and TXSTATS shows AP ok=0. Related: IDFGH-18499 / esp-idf#19267.
Test harness: win11 VM + Hama RTL8188EUS (0bda:8179) as the SoftAP client, SSH trash@192.168.1.153.

## Ruled out
- C5 hardware / bare IDF SoftAP driver: the minimal IDF SoftAP+httpd on the same board works (see below).
- Power save: WIFI_PS_NONE did not help.
- TX buffering: dynamic TX buffers (NUM=64) + CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=n did not help.
- App features: stripped app (CFG_C2_MIN_REPRO: no SD/Display/recon, stock sdkconfig) still wedges: 10/10 curl 000, TXSTATS AP ok=0.
- Arduino WiFi.softAP call sequence alone: replacing it with raw IDF SoftAP (esp_netif_create_default_wifi_ap + WIFI_MODE_AP + DHCP) fixed 0.0.0.0 -> real 192.168.4.1 and beacon "Lily" ch6 HT20, but association still fails (associating -> disconnected, no DHCP, AP ok=0).

## Confirmed working
- /home/op/scratch/c5_softap_min (IDF 5.5.5, SSID Lily): win11+Hama associates; 25x HTTP 200.

## Other observations
- esp_http_server :81 /raw added next to Arduino SoftAP: A/B test blocked because association never completes (AP ok=0 through the 4-way handshake).
- c2min-stock env (stock TX sdkconfig) panics with Guru Meditation on esp_wifi_start. The device was left on c2min (DYNAMIC_TX + AMPDU off).

## Current locus / hypothesis
The wedge is in the Arduino-hosted process, not the SoftAP sequence: the arduino-esp32 runtime and the linked WiFi/Network libs, plus sdkconfig differences (stock panics).
Remaining diffs vs c5_softap_min: Arduino runtime init before SoftAP, linked WiFi/Network libs, sdkconfig, tx_done_cb + dual HTTP servers, PSRAM/MSPI warning, C2 routes.

## Next steps (not started)
(a) Diff sdkconfig and link map/libs between c2min and c5_softap_min; bisect the sdkconfig deltas into the minimal build.
(b) Hybrid: port the lily app logic onto the clean IDF SoftAP env without arduino-esp32 WiFi/Network.

## cursor-agent + model?
Worth it mainly for (a)'s mechanical sdkconfig/link diffing and for porting in (b). Hardware A/B testing still needs a human or tool-driven loop.

---

## 2026-10-31 — Build-config comparison: `c2min` (Arduino) vs `c5_softap_min` (minimal IDF)

Read-only compare of the two effective generated sdkconfigs and linked ELFs. No flash, no rebuild, no commit.

**Artifacts used (both found):**
- Arduino: `firmware/sdkconfig.T-Dongle-C5-c2min` (4499 lines) + `.pio/build/T-Dongle-C5-c2min/firmware.elf` / `.map`
- Minimal: `scratch/c5_softap_min/sdkconfig.c5min` (4083 lines) + `.pio/build/c5min/firmware.elf` / `.map`

### Link / symbol differences
- **Arduino WiFi class IS linked into `c2min`, NOT into the minimal demo.** `nm | c++filt` on `c2min` ELF shows **128** `WiFiGenericClass` / `WiFiSTAClass` / `WiFiScanClass` / `NetworkInterface` / `NetworkEvents` symbols; the minimal ELF has **0**. The minimal demo calls `esp_wifi_*` / `esp_netif` directly.
- `c2min` links the **prebuilt** `framework-arduinoespressif32-libs/esp32c5/lib/*` (incl. `libesp_wifi.a`, `libnet80211`, `libpp`, `libphy`, `libcoexist`, plus `libcore` = Arduino core). The minimal links the **from-source** `framework-espidf/components/esp_wifi/lib/esp32c5/*` (`libnet80211/libpp/libcore/libespnow/...`) + its own `libesp_wifi.a`. So they use **different WiFi/PHY/MAC binary blobs** (prebuilt Arduino libs vs IDF component libs), on top of the Arduino WiFi C++ wrapper. This is a structural difference the pure-config diff cannot capture.

### CONFIG deltas that can plausibly affect SoftAP **association / downlink** (A=`c2min`, B=minimal)
| CONFIG | c2min (A) | minimal (B) | note |
|---|---|---|---|
| `ESP_WIFI_AMPDU_TX_ENABLED` (+`ESP32_WIFI_...`) | **n** | **y** | lily override disabled AMPDU TX; the *working* demo has it ON. Also removes `ESP_WIFI_TX_BA_WIN` (B=6). |
| `ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | **64** | **32** | both use *dynamic* TX (`DYNAMIC_TX_BUFFER=y`, `TX_BUFFER_TYPE=1`) — see correction below |
| `ESP_WIFI_IRAM_OPT` | **n** | **y** | WiFi TX path out of IRAM in Arduino build |
| `ESP_WIFI_RX_IRAM_OPT` | **n** | **y** | WiFi RX path out of IRAM in Arduino build |
| `ESP_WIFI_RX_BA_WIN` | 8 | 6 | minor |
| `ESP_WIFI_STATIC_RX_BUFFER_NUM` | 8 | 10 | minor |
| `SPIRAM` (whole PSRAM stack) | **y** (QUAD, 80M, malloc, stack-ext) | **not set** | biggest structural diff; see caveat |

**Correction to earlier notes:** the minimal demo does **not** use static TX buffers. It uses **dynamic** TX (`DYNAMIC_TX_BUFFER=y`, `TX_BUFFER_TYPE=1`, `DYNAMIC_TX_BUFFER_NUM=32`) **with AMPDU TX enabled**. So the lily `c2min` override ("dynamic TX + AMPDU TX off, num 64") actually moved the config **away** from the known-good minimal in exactly the AMPDU-TX and buffer-count dimensions. The `c2min-stock` env does NOT match the minimal either: dropping the overrides reverts to the arduino-esp32 default (**static** TX, num 8, AMPDU on) — a *third* config, which is also where it Guru-panics at `esp_wifi_start`.

### Deltas that are almost certainly NOISE for *association* (L2 auth/assoc happens below lwIP)
All the LWIP_* differences: `IPV4_NAPT`/`NAPT_PORTMAP`/`IP_FORWARD` (Arduino enables routing/NAT), `TCPIP_CORE_LOCKING` (A=y/B=n), `TCPIP_TASK_AFFINITY` (CPU0 vs none — single-core C5, moot), stack size 4096 vs 3072, `TCP_MSS` 1436 vs 1440, PPP/SLIP, IPv6 autoconfig/DHCPv6/RDNSS, SNTP/DHCP-NTP, `LWIP_STATS` (instrumentation from the #9 override), mDNS-in-SPIRAM. Also flash/MSPI: `FLASHMODE` QIO (A) vs DIO (B) and the PSRAM `PM_SLP_*`/`SPIRAM_*` sub-keys — these ride along with the PSRAM/flash choice and don't touch 802.11 association. `ESP_WIFI_CSI_ENABLED`, `FTM_*`, `GCMP` differ too but are feature toggles, not association-path.

### Cheapest 1–3 config flags to try first (move `c2min` toward the known-good minimal)
1. **Re-enable AMPDU TX** — in `custom_sdkconfig` set `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y` (drop the `=n` override). This is the single clearest divergence from the working demo and is one line.
2. **Match the TX buffer count** — `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32` (not 64), keeping dynamic TX / `TX_BUFFER_TYPE=1` as the minimal demo has them.
3. **Enable WiFi IRAM optimisation** — `CONFIG_ESP_WIFI_IRAM_OPT=y` and `CONFIG_ESP_WIFI_RX_IRAM_OPT=y` (both ON in the demo; OFF in Arduino). Plausible timing link given PSRAM XIP is also on.

Try #1 (+#2, same category) first — they revert exactly the #9 overrides that diverge from the proven-good config and cost nothing. #3 next.

**Caveat on the big one:** `CONFIG_SPIRAM=y` vs not-set is the largest structural config delta, but per issue #18 / AGENTS.md disabling PSRAM on this pioarduino fork *regresses* WiFi init (0x3001), so it is **not** a cheap first toggle — treat as structural, not a quick A/B.

**Still unexplained by config alone:** even with identical WiFi knobs, `c2min` links the Arduino WiFi C++ wrapper + the *prebuilt* arduino-esp32 WiFi/PHY blobs (vs the demo's from-source IDF libs). If flags 1–3 don't restore association, the residual difference is binary (library provenance) + the Arduino init path, i.e. step (b) hybrid.

## 2026-10-31 — step (a) config A/B: align c2min WiFi knobs with c5_softap_min

**Change (env T-Dongle-C5-c2min only, via custom_sdkconfig in firmware/platformio.ini):**
1. CONFIG_ESP_WIFI_AMPDU_TX_ENABLED=y (was n)
2. CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32 (was 64; still dynamic TX)
3. CONFIG_ESP_WIFI_IRAM_OPT=y + CONFIG_ESP_WIFI_RX_IRAM_OPT=y (were unset)
No PSRAM change. Generated sdkconfig.T-Dongle-C5-c2min confirmed all four values.

**Build/flash:** pio run -e T-Dongle-C5-c2min -t upload — SUCCESS. Image left on device: this build (ELF SHA256 prefix fda174cc1).

**Boot result:** SoftAP does **not** come up. After [wifi_init=ok] [mode_ap=ok] [cfg=ok] the device Guru-panics (Load access fault, MCAUSE=5, MTVAL=0x28) and reboots in a loop. Backtrace (addr2line on firmware.elf):
_do_wifi_start -> wifi_start_process -> wifi_softap_start -> ieee80211_hostap_attach (MEPC 0x4208021a). Same locus as earlier c2min-stock Guru at esp_wifi_start.

**Association / DHCP / HTTP / TXSTATS:** not runnable — no beacon, no SoftAP. win11+Hama test skipped.

**Baseline comparison:** previous c2min (DYNAMIC=64, AMPDU TX off, IRAM opts off) booted SoftAP (SSID Lily, 192.168.4.1) but win11 failed to associate (TXSTATS AP ok=0, curl 000). Moving the three knobs toward the known-good minimal **regresses boot** to a Guru, same class as c2min-stock.

**Conclusion:** config alone (within the Arduino/prebuilt-blob link) is not the fix — aligning with the demo AMPDU/TX-count/IRAM values crashes SoftAP start. Residual difference is **Arduino WiFi wrapper + prebuilt arduino-esp32 WiFi/PHY blobs** (vs demo from-source IDF libs) -> proceed to step **(b)** hybrid (lily app on clean IDF SoftAP env without arduino-esp32 WiFi/Network). Base lily env still has AMPDU=n / TX=64; only c2min was patched for this A/B.

## 2026-10-31 — step (b) layer 2: C2 HTTP handlers on clean IDF SoftAP

**Image:** /home/op/scratch/c5_hybrid_l2 (copy of c5_softap_min + c2_server.c/.h + lily_dashboard.h; SoftAP init untouched; zero Arduino WiFi). Built with DeepSeek v41 via cursor-agent. Flashed: pio run -t upload SUCCESS. Device left on this image.

**Boot:** clean. SoftAP_UP ssid=Lily ch=6 ip=192.168.4.1; C2 routes registered (16 handlers); HTTPD_UP :80; periodic `alive sta=N`. No Guru Meditation, no reboot loop. Auth token printed to serial (AMTFMYM73RKBSU2A this boot).

**Association (win11 + Hama RTL8188EUS 0bda:8179):** SUCCESS.
- netsh connect name=Lily → State=connected, BSSID 38:44:be:bc:f9:3d, ch6, 802.11n, WPA2-Personal/CCMP, signal 100%.
- DHCP: Wi-Fi got 192.168.4.2/24, gateway/DNS 192.168.4.1.
- Device-side: `alive sta=1 heap=186068` (was sta=0 before connect).

**HTTP (from win11 over SoftAP):**
- `/api/status` ×10: all 200, body `{"state":"no_interp","clients":1,"ap_ip":"192.168.4.1"}`
- `/raw` ×3+: all 200, body `raw-ok`
- `/` dashboard: 200, SIZE=7217 (LilyDucky HTML)
- ping 192.168.4.1: 4/4 replies, ~5–12 ms, 0% loss

**TXSTATS:** hybrid firmware does not expose the lily TXSTATS serial command (no tx_done_cb). Proxy metric: `alive sta=1` + sustained HTTP 200 / ping OK implies AP→STA TX is working (unlike c2min where AP ok=0 and curl 000).

**Baseline comparison:** behaves like **c5_softap_min** (associates + HTTP 200), **not** like lily/c2min (association fail, AP ok=0). Adding the C2 HTTP layer (esp_http_server handlers + dashboard, still on from-source IDF SoftAP) does **not** break association.

**Verdict:** layer 2 clean — proceed to layer 3 (app features: SD/Display/recon as no-op or real). Residual SoftAP wedge remains in the Arduino WiFi wrapper / prebuilt arduino-esp32 WiFi blobs, not in C2 HTTP itself.

## 2026-11-04 — layer 3a: SD card storage + payload file handling (pure ESP-IDF)

**Image:** /home/op/scratch/c5_hybrid_l3a (copy of c5_hybrid_l2). Ported lily's SD
storage + payload file endpoints onto the clean IDF hybrid, pure ESP-IDF only
(no arduino-esp32, no PSRAM). Built via cursor-agent (deepseek-v41-flash-1017-high)
in a herdr pane; verified independently afterwards. NOT flashed. No commit.

**New files:** src/storage.c, src/storage.h — SD-over-SPI (esp_vfs_fat_sdspi_mount,
SPI2_HOST, 20 MHz) mounted RW at /sdcard. Implements the six c2_hooks_t payload
hooks (listPayloads/payloadCount/payloadName/payloadSize/readPayload/writePayload)
over /sdcard/payloads (created if missing). Missing card is non-fatal: logs a
warning, HTTP keeps serving.

**SD pins (T-Dongle-C5, SPI mode, from firmware/src/config.h):** SCLK=GPIO6,
MOSI=GPIO2, MISO=GPIO7, CS=GPIO23 (shared LCD/SD bus). format_if_mount_failed off
(never repartitions a user card).

**main.c:** additive only — a static c2_hooks_t with the payload hooks set,
getInterpState/runPayload/stopPayload and all recon* left NULL; register call now
passes &hooks; storage_init() after httpd up with non-fatal warning. WiFi/SoftAP
code byte-identical. src/CMakeLists.txt: added storage.c + REQUIRES fatfs vfs
sdmmc esp_driver_sdspi esp_driver_spi esp_driver_gpio. c2_server.c/.h unchanged.

**sdkconfig:** sdkconfig.defaults gained only CONFIG_FATFS_LFN_HEAP=y and
CONFIG_FATFS_MAX_LFN=64. platformio.ini identical to l2. Verified: all
CONFIG_ESP_WIFI / ESP32_WIFI / LWIP / SPIRAM keys in generated sdkconfig.c5min are
IDENTICAL to l2; CONFIG_SPIRAM still "not set" (issue #18 safe).

**Build:** pio run -e c5min — SUCCESS (6.4 s incremental; clean build 29.5 s).
RAM 13.4% (44044/327680 B), Flash 6.3% (1058830/16777216 B).
firmware.bin = /home/op/scratch/c5_hybrid_l3a/.pio/build/c5min/firmware.bin,
1,059,440 bytes. Not flashed.

**Still stubbed (NULL hooks):** /api/run, /api/stop, interpreter, display, all
recon — later layers.

**Concerns / to test on hardware next session:** (1) SD bus (GPIO2/6) is shared
with the APA102 LED and LCD; layer 3a drives SD only, but once LED/LCD land they
must arbitrate the shared SPI bus (lily re-latches the LED after bus traffic).
(2) Association + /api/payloads/<name> GET/PUT not yet tested on win11+Hama —
build-only so far; retest association after flashing.

## 2026-11-04 — Layer 3a flash FAIL (partition overflow); l2 restored PASS

### Layer 3a flash attempt
- Flashed /home/op/scratch/c5_hybrid_l3a via pio run -e c5min -t upload --upload-port /dev/ttyACM0 — esptool SUCCESS, hash verified.
- Boot capture (probe_reset.py): boot loop. Key lines:
  - E esp_image: Image length 1059440 does not fit in partition length 1048576
  - E boot: Factory app partition is not bootable
  - E boot: No bootable app partitions in the partition table
- Root cause: default factory partition is 1 MiB (0x100000); l3a firmware.bin is 1,059,440 B (overflow by ~10.7 KiB). l2 was 945,696 B and still fit.
- No SoftAP, no C2, no SD mount reached — image never started. No Guru (never entered app).
- SD mount status: N/A (app not bootable).

### Recovery
- Immediate pio/esptool reflash of l2 failed while the device was boot-looping (USB-JTAG connect occasional, stub flasher Unexpected response empty).
- Recovered with OpenOCD esp32c5-builtin (downloaded openocd-esp32 v0.12.0-esp32-20260831) while holding RTS to stabilize: programmed l2 bootloader + partitions + firmware, verify OK.
- Device left on c5_hybrid_l2 image.

### Post-recovery win11 + Hama retest (l2)
- SoftAP UP: ssid=Lily ch=6 ip=192.168.4.1, C2 routes registered: 16 handlers, HTTPD_UP :80, alive sta=0, no Guru.
- win11 Hama (RTL8188EU) associated; DHCP 192.168.4.3.
- HTTP 10x: /api/status 200, /raw 200, / 200, /api/payloads 200 (l2 stub).
- ping 192.168.4.1: 0% loss.

### Next for Layer 3a
1. Grow factory partition (e.g. 2 MiB CSV + board_build.partitions) before flashing any build >=1 MiB.
2. Rebuild l3a against the larger table, flash, retest association after SD init.
3. Prefer OpenOCD USB-JTAG recovery path if a future oversized image bricks boot again.


## 2026-11-04 (later) — Layer 3a PASS after 2 MiB factory partition

### Change (c5_hybrid_l3a only)
- Added partitions.csv: nvs 0x9000/0x6000, phy_init 0xf000/0x1000, factory 0x10000/0x200000 (2 MiB), no OTA.
- platformio.ini: board_build.partitions = partitions.csv
- sdkconfig.defaults: CONFIG_PARTITION_TABLE_CUSTOM=y + CUSTOM_FILENAME=partitions.csv
- WiFi/LWIP/SPIRAM sdkconfig keys vs l2: IDENTICAL (220/220).

### Build / flash
- Clean rebuild SUCCESS. firmware.bin still 1,059,440 B — fits 2 MiB.
- Full esptool upload (bootloader+partitions+app) SUCCESS; OpenOCD not needed.

### Boot (probe_reset.py)
- Partition table shows factory length 00200000.
- SoftAP Lily 192.168.4.1, C2 16 handlers, HTTPD :80.
- storage: SD mounted at /sdcard (SDHC ~30 GB, 20 MHz).
- No Guru. alive sta=1 (client already present during capture).

### win11 + Hama
- Associated; DHCP 192.168.4.4.
- HTTP 10x (see session log): /api/status, /raw, /, /api/payloads — all 200.
- PUT /api/payload/l3atest.txt with X-Auth-Token -> 200; GET same -> body l3a-test-payload-20261104 (200); list shows l3atest.txt size 25 plus existing .dd files.
- ping 192.168.4.1: 0% loss.

### Device left on
- c5_hybrid_l3a (2 MiB factory partition image).


## 2026-11-04 — Layer 3b: interpreter + /api/run + /api/stop (build only, not flashed)

### Tree
- Copied working l3a -> /home/op/scratch/c5_hybrid_l3b (kept 2 MiB factory partitions.csv).
- cursor-agent deepseek-v41-flash-1017-high in herdr tab layer3b, ~15 min, rc=0.
- Prompt: LAYER3B_PROMPT.txt; log: /home/op/scratch/c5_hybrid_l3b.agent.log (partial final summary only; stdout buffered).

### Build
- pio run -e c5min SUCCESS.
- RAM 16.8% (54940 / 327680 B), Flash 6.4% (1071706 / 16777216 B).
- firmware.bin = 1,072,320 B (51.1% of 2 MiB factory) — no partition grow needed.

### Files added/changed (l3b only)
- NEW: src/interpreter.cpp, src/interpreter.h — ESP-IDF DuckyScript port (no Arduino).
- NEW: src/interp_hal.c/.h — press/release no-ops (C5 has no USB-OTG HID).
- NEW: src/duckcfg.h — CFG_* constants needed by the interpreter.
- EDIT: src/main.c — wire getInterpState / runPayload / stopPayload; duck_interp_init(); storage hooks unchanged.
- EDIT: src/CMakeLists.txt — added interpreter.cpp, interp_hal.c, REQUIRES esp_timer.
- UNCHANGED vs l3a: c2_server.c/.h, storage.c/.h, partitions.csv, sdkconfig.defaults.
- WiFi/LWIP/SPIRAM sdkconfig keys vs l3a: IDENTICAL (220/220). CONFIG_SPIRAM still unset.

### Hooks now
- Wired: getInterpState, runPayload, stopPayload (+ all storage_* from 3a).
- Still stubbed/NULL: all recon*, display.

### Concerns
- HID is no-op on C5 by design; scripts advance delays/control flow but type nothing.
- DETECT_OS / ATTACKMODE/MSC paths are no-op/unknown-safe.
- Host-side interpreter harnesses used during the port lived under /tmp and are ephemeral.
- Not flashed yet — SoftAP association retest required after flash approval.

## 2026-11-04 — Layer 3b flash + HW PASS (interpreter run/stop)

### Flash
- Full `pio run -e c5min -t upload --upload-port /dev/ttyACM0` from /home/op/scratch/c5_hybrid_l3b SUCCESS (bootloader + 2 MiB partitions + app).
- firmware.bin 1,072,320 B. esptool verified; hard reset via RTS. OpenOCD fallback not needed.

### Boot (probe_reset.py → /tmp/lily_probe_l3b.log)
- Partition table: factory length 00200000 (2 MiB).
- SoftAP Lily ch=6 ip=192.168.4.1 (stock IDF softAP+httpd).
- interp: interpreter task started (HID: none on C5, keys are no-ops).
- C2 routes registered: 16 handlers; HTTPD_UP :80.
- storage: SD mounted at /sdcard (SDHC SH32G ~30 GB, 20 MHz).
- No Guru / panic. alive sta=1 heap≈127228 during capture.
- Auth token (session): ZQP62CEG63AP3M4M (printed on serial; required for mutating routes).

### win11 + Hama (RTL8188EU)
- Associated to Lily; DHCP 192.168.4.6.
- HTTP 10× each: /api/status, /raw, /, /api/payloads — TOTAL_OK=40 FAIL=0 (all 200).
- Functional interpreter:
  - GET /api/status before → state=idle.
  - PUT /api/payload/l3btest.txt (DELAY 8000 / STRING hello / DELAY 8000) + X-Auth-Token → 200 OK.
  - POST /api/run/l3btest.txt → 200 Running.
  - Polled /api/status ×8 → state=running throughout.
  - POST /api/stop → 200 Stopped; subsequent polls → state=stopped.
  - HID keystrokes are no-ops on C5 by design; verified interpreter state machine only.
- ping 192.168.4.1: 4/4 OK, rtt 3–8 ms.

### Device left on
- c5_hybrid_l3b (2 MiB factory; SoftAP + C2 + SD + interpreter).
- Known-good restore image if needed: c5_hybrid_l3a (not required this run).

### Next
- Layer 3 recon/display still stubbed — do not flash further layers without SoftAP retest.
- SoftAP/WiFi/LWIP must stay untouched when adding recon.

## 2026-11-04 — Layer 3c: LCD/display + recon (build only, not flashed)

### Tree
- Copied working l3b -> /home/op/scratch/c5_hybrid_l3c (2 MiB factory kept).
- cursor-agent deepseek-v41-flash-1017-high in new herdr tab `layer3c`, ~37 min, rc=0.
- Prompt: LAYER3C_PROMPT.txt; log: /home/op/scratch/c5_hybrid_l3c.agent.log (brief reconnect mid-run, recovered).

### Build (verified `pio run -e c5min` SUCCESS)
- RAM 23.1% (75640 / 327680 B), Flash 6.6% (1105638 / 16777216 B).
- firmware.bin = 1,106,288 B (52.8% of 2 MiB factory) — no partition grow.
- partitions.csv / sdkconfig.defaults unchanged vs l3b.
- WiFi/LWIP/SPIRAM sdkconfig.c5min keys vs l3b: IDENTICAL (247/247). CONFIG_SPIRAM still unset.

### Files added/changed (l3c only)
- NEW: board_pins.h, board_spi.c/.h — shared SPI2_HOST owner + recursive mutex + APA102 relatch.
- NEW: st7735.c/.h — minimal pure-IDF ST7735 driver (no TFT_eSPI / Arduino).
- NEW: duck_display.c/.h — diff-gated status dashboard (~1 Hz from app_main).
- NEW: recon.c/.h, recon_config.h, pcap_writer.c/.h — SoftAP-safe capture/scan/enum.
- EDIT: main.c — wire all recon* hooks; board_spi + display init; display tick in alive loop.
- EDIT: storage.c — uses board_spi_bus_init / add_device; no longer owns/frees the bus.
- EDIT: CMakeLists.txt — new SRCS + esp_driver_ledc.
- EDIT: interpreter.cpp/.h — read-only progress snapshot for the dashboard.
- UNCHANGED byte-identical vs l3b: c2_server.c/.h.

### Hooks now
- Wired: getInterpState, run/stopPayload, all storage_*, ALL recon*
  (start/stop/capturing/file/packet/dropped, busy/scan/scanning/lastScanMs/apCount/apRecord,
  startEnum/enumRunning/staCount/staRecord).
- Display is local (not a c2_hooks_t entry): duck_display_init + duck_display_update.
- Still out of scope / stubbed: AP-down PMF sweep (recon_start_pmf_sweep_ap_down -> false forever).
- Note: GET /api/recon/pcap/<name> still 404 — c2_hooks_t has no pcap-read hook and c2_server was left untouched; files are written to /sdcard/recon.

### SPI bus sharing
- board_spi_bus_init() is the sole spi_bus_initialize(SPI2_HOST) caller; never frees the bus (SD mount fail no longer kills LCD).
- Separate devices: SDSPI CS=23, ST7735 CS=10 (DC=3 RST=1 BL=0 active-LOW), APA102 CS=-1 on MOSI=2/SCK=6.
- Recursive mutex wraps multi-transaction LCD frames and SD/PCAP bursts; board_led_relatch() after paint/SD/recon bus use.

### SoftAP / scan risk
- C2 scan defaults keepApUp; AP->APSTA only when needed; home_chan_dwell_time=100 ms; restore delayed 300 ms with channel-before-mode.
- Capture pinned to live SoftAP channel (esp_wifi_get_channel).
- Enum intentionally dwells off-channel ~4 s/attempt (up to 2 attempts) — highest SoftAP risk of the three modes; gated by recon_busy().
- No AP-down SoftAP teardown path.

### Concerns (pre-flash)
- LCD unproven on hardware (offsets/BGR/inversion match lily TFT_eSPI GREENTAB160x80; blank panel possible until HW test).
- Enum can blip win11+Hama clients; capture+scan should be safer.
- PCAP download via REST still missing (needs a new c2 hook + tiny c2_server change later).
- NOT flashed — SoftAP association + LCD + recon HW retest required after flash approval.

## 2026-11-04 evening wrap
Session paused for the night. Layer 3c is built and documented above but was **not flashed**. Device remains on c5_hybrid_l3b (PASS). Continue tomorrow from `docs/knowledge-base/issue-9-RESUME.md` — flash l3c only after Kasper approval, then win11+Hama SoftAP + LCD + recon/enum under active SoftAP.

## 2026-11-05 — Layer 3c flash + HW PASS (display + recon under SoftAP)

### Flash
- Full  from
  /home/op/scratch/c5_hybrid_l3c SUCCESS (bootloader + 2 MiB partitions + app).
- firmware.bin 1,106,288 B. esptool verified; hard reset via RTS.
  OpenOCD fallback not needed. Restore of l3b not required.

### Boot (probe_reset.py → /tmp/lily_probe_l3c.log, ~12 s)
- Partition table: factory length 00200000 (2 MiB).
- SoftAP Lily ch=6 ip=192.168.4.1 (stock IDF softAP+httpd).
- board_spi: SPI2 shared bus up; SD mounted at /sdcard (SH32G ~30 GB, 20 MHz).
- st7735: ST7735 up 160x80 landscape; display: dashboard up (ssid=Lily).
- recon: ready (capture on AP channel, scan AP-up, no AP-down sweep).
- boot complete (display=up, recon=idle, sd=mounted). No Guru / panic.
- C2 routes registered: 16 handlers; HTTPD_UP :80.
- Auth token (session): CHFPRRB25DUVLR36 (serial; mutating routes).

### win11 + Hama (RTL8188EU 80:3f:5d:13:3b:34)
- Connected to Lily profile; DHCP 192.168.4.3; AP BSSID 38:44:be:bc:f9:3d ch=6;
  signal 100% / rssi -19; rate 72.2 Mbps.
- HTTP codes (all 200 unless noted):
  | Route | Code | Notes |
  |-------|------|-------|
  | GET / | 200 | dashboard HTML |
  | GET /api/status | 200 | state=idle, clients=2 |
  | GET /raw | 200 | raw-ok |
  | GET /api/payloads | 200 | l3a/l3b test files present |
  | GET /api/recon/status | 200 | capturing=false |
  | GET /api/recon/scan | 200 | before/after scan |
  | GET /api/recon/enum | 200 | |
  | POST /api/recon/scan (auth) | 200 | {"scanning":true,"apUp":true} |
  | POST /api/recon/start (auth) | 200 | file=recon_198191.pcap |
  | POST /api/recon/stop (auth) | 200 | Stopped; mid-capture packets=20 dropped=0 |
  | POST /api/recon/enum/0 (auth) | 200 | {"enumerating":true} → 0 stations |
  | GET /api/recon/pcap/nosuch.pcap (auth) | **404** | known stub in c2_server (no SD stream) |
  | GET /api/recon/pcap/... (no auth) | 401 | auth-gated |

### SoftAP risk under recon (deliberate)
- **Scan while SoftAP up:** completed; 13–14 APs reported across ch 1–13.
  Hama stayed **connected**; ping 2/3 then recovered (one timeout mid-scan —
  brief blip, no drop).
- **Capture on AP channel:** 20 packets, Hama stayed connected; ping 3/3.
- **Enum index 0 (~4 s off-channel dwell):** ran to completion (0 stations on
  that AP). Hama stayed **connected**; ping **4/4** after enum. No association
  drop observed on this run.
- Final /api/status 200, clients=2, ping 4/4 @ ~6 ms.

### LCD
- Firmware path: ST7735 init + duck_display dashboard reported up on serial.
- Physical panel **unobserved** from this path (no camera / SPICE view of the
  dongle). Treat HW panel paint as still unverified by eye.

### Device left on
- c5_hybrid_l3c (2 MiB factory; SoftAP + C2 + SD + interpreter + display + recon).
- Known-good restore if needed: /home/op/scratch/c5_hybrid_l3b.

### Remaining gaps
- GET /api/recon/pcap/<name>: stub closed in c5_hybrid_pcap (built, not yet flashed; see Layer pcap below).
- LCD GREENTAB paint not visually confirmed.
- AP-down PMF sweep still stubbed.
- Enum returned 0 stations on tested index (radio path exercised; client sniff
  content not proven rich).

## Layer pcap — GET /api/recon/pcap/<name> (2026-11-05)
- Tree: `/home/op/scratch/c5_hybrid_pcap` (copy of l3c). Build only; **not flashed**.
- cursor-agent deepseek-v41-flash-1017-high in herdr tab `layer-pcap`, pio SUCCESS.
- `firmware.bin` 1,106,928 B (52.78% of 2 MiB factory). RAM ~23.1%.
- Added `storage_read_recon()` (binary-safe SD read of `/sdcard/recon/<name>`),
  `c2_hooks_t::reconReadPcap`, wired `handle_recon_get_pcap` to serve
  `application/vnd.tcpdump.pcap` after existing `auth_ok` + `valid_name`.
- Files touched: `storage.c/.h`, `c2_server.c/.h`, `main.c` only.
- `sdkconfig.c5min` / `partitions.csv` unchanged vs l3c; `# CONFIG_SPIRAM is not set`.
- SoftAP/WiFi/LWIP path untouched. Next: flash + win11+Hama download retest.

## 2026-11-05 — Layer pcap: GET /api/recon/pcap/<name> flash + HW PASS

### Flash
- Tree: /home/op/scratch/c5_hybrid_pcap (copy of l3c + storage_read_recon / reconReadPcap hook).
- `pio run -e c5min -t upload` SUCCESS via esptool on /dev/ttyACM0 (~14.7 s).
- firmware.bin 1,106,928 B (52.78% of 2 MiB factory). RAM 23.1%.
- OpenOCD fallback not needed. Restore of l3c not required.

### Boot (probe_reset.py → /tmp/lily_probe_pcap.log)
- SoftAP Lily ch=6 ip=192.168.4.1; HTTPD_UP :80.
- display dashboard up; recon ready; boot complete (display=up, recon=idle, sd=mounted).
- Auth token (session): QKLD67PMKULRR3D8 (serial; mutating + pcap GET).
- No Guru / panic.

### win11 + Hama (RTL8188EU 80:3f:5d:13:3b:34)
- Reconnected to Lily profile after flash; DHCP 192.168.4.3; AP BSSID 38:44:be:bc:f9:3d ch=6;
  signal 100% / rssi -22; rate 72.2 Mbps.
- HTTP:
  | Route | Code | Notes |
  |-------|------|-------|
  | GET /api/status | 200 | state=idle, clients=2, ap_ip=192.168.4.1 |
  | GET /api/recon/status | 200 | capturing=false initially |
  | POST /api/recon/start (auth) | 200 | file=recon_67329.pcap |
  | GET /api/recon/status (mid) | 200 | capturing=true, packets=73, dropped=0 |
  | POST /api/recon/stop (auth) | 200 | Stopped; packets=74 dropped=0 |
  | GET /api/recon/pcap/recon_67329.pcap (auth) | **200** | Content-Type application/vnd.tcpdump.pcap; Content-Length 25051; body magic d4:c3:b2:a1 (libpcap LE) |
  | GET /api/recon/pcap/recon_67329.pcap (no auth) | 401 | auth-gated |
  | GET /api/recon/pcap/nosuch.pcap (auth) | 404 | Not found |

### SoftAP health after download
- Hama stayed **connected** to Lily throughout capture + 25 KiB download.
- ping 192.168.4.1: 4/4 OK, rtt 3–15 ms.
- Final /api/status 200, clients=2.

### Device left on
- c5_hybrid_pcap (2 MiB factory; SoftAP + C2 + SD + interpreter + display + recon + pcap download).
- Known-good restore if needed: /home/op/scratch/c5_hybrid_l3c.

### Gap closed
- GET /api/recon/pcap/<name> no longer a stub; serves SD /sdcard/recon/<name> via storage_read_recon + reconReadPcap hook.
- Remaining: LCD GREENTAB paint not visually confirmed; AP-down PMF sweep still stubbed; fold hybrid into lily_ducky main tree and mark #9 resolved.

---

## 2026-11-05 — MERGED INTO THE MAIN TREE (`T-Dongle-C5-idf`)

The scratch-tree hybrid is now part of `lily_ducky/firmware/`. This closes the
last remaining item in the list above and settles the root-cause question.

### Root cause, stated plainly
The AP→STA downlink wedge lived in the **arduino-esp32 WiFi wrapper together
with the prebuilt Arduino WiFi/PHY/MAC blobs it links**. Two independent
findings say so:

1. **Bisect by config failed in the right direction.** Every sdkconfig knob that
   differed was aligned toward the working minimal IDF demo (AMPDU TX on,
   dynamic TX count 32, WiFi IRAM opts on) — and that *regressed* the Arduino
   build from "associates, then wedges" to a **Guru-panic inside
   `ieee80211_hostap_attach` at `esp_wifi_start`**. Config alone cannot fix it,
   and pushing config toward the good build actively breaks boot.
2. **Link-level difference is structural.** The Arduino build links
   `libesp_wifi.a`/`libnet80211`/`libpp`/`libphy` from
   `framework-arduinoespressif32-libs` plus 128 `WiFiGenericClass`/`WiFiSTAClass`
   symbols; the from-source IDF build links `framework-espidf`'s own WiFi
   components and has zero Arduino WiFi symbols — i.e. **different WiFi/PHY
   binaries**, not merely different config.

Replacing both halves (Arduino runtime WiFi wrapper *and* the Arduino WiFi
blobs) with plain `esp_netif` + `esp_wifi` from source made the same board
associate and serve HTTP indefinitely: L2 (assoc/EAPOL) → L3 (DHCP) → L3c
(HTTP, SD, interpreter, LCD, recon) → PCAP GET, each layer validated on
win11 + Hama.

### What was merged, and where
- **Env:** `T-Dongle-C5-idf` — `framework = espidf`, pioarduino platform
  55.03.311 (same zip as every other env), board `Lilygo-T-Dongle-C5`.
- **Sources:** `firmware/src/idf_softap/` (with its own README) —
  `app_main.c`, `c2_server.c/.h`, `board_spi.c/.h`, `board_pins.h`,
  `storage.c/.h`, `recon.c/.h`, `recon_config.h`, `pcap_writer.c/.h`,
  `st7735.c/.h`, `duck_display.c/.h`, `interpreter.cpp/.h`, `interp_hal.c/.h`,
  `duckcfg.h`, `lily_dashboard.h`, `softap_secret.h`.
- **Build wiring:** `firmware/CMakeLists.txt`, `firmware/src/CMakeLists.txt`,
  `firmware/idf_softap_partitions.csv`, `firmware/idf_softap_sdkconfig.defaults`.
- **Credentials:** `softap_secret.h` includes the existing gitignored
  `firmware/src/config_secret.h`. The hybrid's one-off `src/secret.h` was NOT
  copied — no second copy of the secret enters the repo.
- **Arduino isolation:** `build_src_filter` excludes `idf_softap/` in every
  Arduino env, so `T-Dongle-C5`, `T-Dongle-S3`, the self-test and the LED diags
  are untouched (both main envs rebuild green).

### Why `idf_softap_sdkconfig.defaults` instead of `custom_sdkconfig`
PlatformIO's espidf builder sets `flag_custom_sdkonfig = False` whenever
`espidf` is in `PIOFRAMEWORK`, i.e. **`custom_sdkconfig` is silently ignored for
`framework = espidf`**. The keys therefore have to reach CMake as
`-DSDKCONFIG_DEFAULTS=...` (via `board_build.cmake_extra_args`), which is the
mechanism IDF's own `project.cmake` reads. The file is not named
`sdkconfig.defaults` because the Arduino environments auto-generate that exact
path and it carries `CONFIG_SPIRAM=y`.

Note for anyone editing it: PlatformIO only re-runs CMake when
`sdkconfig.defaults`/CMake files change, so after touching
`idf_softap_sdkconfig.defaults` you must delete `sdkconfig.T-Dongle-C5-idf` or
run `pio run -e T-Dongle-C5-idf -t clean`, or the edit is silently ignored.

### Build + equivalence evidence (pre-flash; later flashed — see Result below)
- `pio run -e T-Dongle-C5-idf` → **SUCCESS**, clean compile of all ten ported
  TUs with no warnings from them.
- `firmware.bin` **1,106,944 B** (52.8% of the 2 MiB factory partition);
  RAM **75,640 B / 327,680 B (23.1%)** — identical to the hardware-passed
  hybrid, which is a good sign the port carries no accidental extra state.
- Partition table decoded from the built `partitions.bin`: `nvs 0x9000/0x6000`,
  `phy_init 0xf000/0x1000`, `factory 0x10000/0x200000` — identical to the
  hybrid's flashed table.
- **145/145 WiFi + LWIP sdkconfig keys byte-identical** to
  `c5_hybrid_pcap/sdkconfig.c5min`. Whole-file diff of every `CONFIG_*`: the
  only difference is the partition CSV filename.
- `# CONFIG_SPIRAM is not set` (issue #18 requirement held); zero
  `CONFIG_ARDUINO*` keys; zero Arduino WiFi symbols in `firmware.map`.
- Ported sources are byte-identical to the hybrid's except: the `app_main.c`
  provenance header, the log tag (`c5min` → `idf_softap`), and the
  `secret.h` → `softap_secret.h` include in two files.

### Remaining (optional; #9 itself is closed)
- LCD GREENTAB paint still not visually confirmed by eye.
- AP-down PMF sweep still stubbed (`recon_start_pmf_sweep_ap_down` → false).
- No serial console commands in this build (Arduino `src/console/` only).
- Arduino `T-Dongle-C5` SoftAP intentionally kept as the #9 repro / A/B contrast; lab SoftAP is only `T-Dongle-C5-idf` (do not use Arduino SoftAP for real lab C2).

## Result — main-tree T-Dongle-C5-idf SoftAP regress (2026-11-05) — #9 FORMALLY CLOSED / RESOLVED
Merged env `T-Dongle-C5-idf` (espidf SoftAP hybrid in `firmware/src/idf_softap/`) was flashed to the T-Dongle-C5 and SoftAP-regressed on win11+Hama to the same bar as prior `c5_hybrid_pcap` PASS: association+DHCP **192.168.4.6**, HTTP 200 on `/`, `/raw`, `/api/status` (ap_ip 192.168.4.1), `/api/payloads`, ping hold, recon scan and enum without SoftAP drop, capture start/stop hold, authenticated pcap download 200 with libpcap Content-Type/magic, 401 without auth, 404 for missing name. Device left on this image; restore not needed.

**Issue #9 is formally closed / RESOLVED** after this main-tree flash PASS (root cause = Arduino WiFi wrapper + prebuilt WiFi/PHY blobs; fix = `T-Dongle-C5-idf`). Evidence trail in this file and `issue-9-RESUME.md` / `open-questions.md` §9 is retained.
