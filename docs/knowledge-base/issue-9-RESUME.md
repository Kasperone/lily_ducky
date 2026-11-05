# Issue #9 SoftAP — RESUME handoff (2026-11-05)

## WHERE WE LEFT OFF (read this first — 2026-11-05 evening Europe/Warsaw)

**SoftAP #9 is RESOLVED and merged.** Hardware work on lily SoftAP paused here
so the lab could move to CERT.PL bug-bounty setup. Pick up from this section;
do not re-litigate the Arduino SoftAP wedge.

| Item | State |
|------|--------|
| Fix | Env **`T-Dongle-C5-idf`** (`framework = espidf`), sources `firmware/src/idf_softap/` |
| Flash / regress | **PASS** win11 + Hama RTL8188EUS — DHCP, HTTP, recon scan/enum/capture, auth-gated pcap GET 200 (libpcap magic); LCD OK on hybrid/`c5_hybrid_pcap` path |
| Merged | `cf8dee6` (*fix SoftAP #9…*) via **PR #28** → `main` **`6fae5bc`** (pushed) |
| Arduino SoftAP | Kept as **#9 repro / A/B** only — branch **`docs/arduino-c5-softap-repro`** @ **`8181303`** (pushed). Optional PR merge into `main` still **pending** |
| Device left on | Main-tree **`T-Dongle-C5-idf`** / hybrid-pcap-class image (restore of scratch layers not needed) |
| Not started | Roadmap **Phase 3 BLE HID**; porting **PMFDOWN** onto IDF SoftAP (still stubbed `false`); further hybrid beyond pcap |
| Pause reason | CERT.PL / hack.cert.pl bug-bounty workspace setup (separate from this repo) |

**Constraint:** short serial only — never leave `scripts/serial_monitor.py` long-running; prefer `probe_reset.py` / short captures. Reach guest via laptop **vm-gx** as user `op` (never SSH into arch-pentest; never touch nftables/SSH/VPN).

### Resume commands (env name = `T-Dongle-C5-idf`)

```bash
cd /home/op/Documents/projects/lily_ducky
git fetch origin && git checkout main && git pull --ff-only   # SoftAP fix is on main @ 6fae5bc
cd firmware
pio run -e T-Dongle-C5-idf                   # build only; flash only if Kasper asks
# Lab SoftAP / real C2: ONLY T-Dongle-C5-idf — do not flash Arduino T-Dongle-C5 SoftAP for lab work
# Optional later: open/merge PR from docs/arduino-c5-softap-repro (8181303) into main
```

Detail trail: `issue-9-softap-findings.md` + open-questions.md §9. Scratch layer PASS table is below.

---

## Problem recap
lily_ducky SoftAP on the T-Dongle ESP32-C5 (Arduino / arduino-esp32 build) fails
AP->STA below lwIP: a win11 + Hama RTL8188EUS (0bda:8179) client cannot associate
and gets no HTTP. Related: IDFGH-18499 / esp-idf#19267.

## Root cause (isolated)
The wedge is the **arduino-esp32 WiFi wrapper + prebuilt WiFi/PHY blobs**, not
config, not SoftAP call sequence, and not the C2 app. Minimal from-source IDF
SoftAP (c5_softap_min) associates + HTTP 200; hybrid layers on that base pass.

## Status: RESOLVED + MERGED into the main firmware tree (2026-11-05)
The hybrid firmware is no longer a scratch-tree experiment. It is ported into
`lily_ducky/firmware/` and buildable from the repo.

**New PlatformIO env: `T-Dongle-C5-idf`** — `framework = espidf` (no Arduino
core, no Arduino WiFi), same pioarduino 55.03.311 platform zip as every other
env, board `Lilygo-T-Dongle-C5`.

**Sources: `firmware/src/idf_softap/`** (directory README there):
- `app_main.c` — raw `esp_netif` + `esp_wifi` SoftAP (`Lily`, 192.168.4.1, ch6),
  the `c2_hooks_t` table wiring every subsystem, 1 Hz display/alive loop
- `c2_server.c/.h` — 16 `esp_http_server` handlers incl. `GET
  /api/recon/pcap/<name>`; `lily_dashboard.h` is the HTML
- `board_spi.c/.h` + `board_pins.h` — sole SPI2_HOST owner, bus mutex, APA102
  re-latch
- `storage.c/.h` — SD over SPI, FATFS `/sdcard`, payloads + binary PCAP read
- `recon.c/.h`, `recon_config.h`, `pcap_writer.c/.h` — capture/scan/enum
- `st7735.c/.h` + `duck_display.c/.h` — pure-IDF LCD + dashboard
- `interpreter.cpp/.h`, `interp_hal.c/.h`, `duckcfg.h` — DuckyScript VM
- `softap_secret.h` — reuses the existing gitignored
  `firmware/src/config_secret.h`; no second copy of the credentials

**Build wiring added at `firmware/`:**
- `CMakeLists.txt` (root) and `src/CMakeLists.txt` (explicit source list — no
  globbing that would pull in the Arduino tree)
- `idf_softap_partitions.csv` — 2 MiB factory, no OTA (image ~1.11 MB). The
  Arduino 3 MiB OTA table is untouched.
- `idf_softap_sdkconfig.defaults` — passed to CMake as
  `-DSDKCONFIG_DEFAULTS=idf_softap_sdkconfig.defaults`, because PlatformIO
  **ignores `custom_sdkconfig` for `framework = espidf`**. This is also why it
  isn't named `sdkconfig.defaults` (the Arduino-generated file owns that name
  and carries `CONFIG_SPIRAM=y`).
- `platformio.ini`: new env + `-<idf_softap/>` added to every Arduino
  `build_src_filter`.

**Build result (2026-11-05):** `pio run -e T-Dongle-C5-idf` **SUCCESS, clean**
(no warnings from the new sources). `firmware.bin` **1,106,944 B** = 52.8% of
the 2 MiB factory partition. RAM **75,640 B (23.1%)** — byte-identical to the
hardware-passed hybrid image.

**Config equivalence to the known-good image — verified:**
- All **145/145 WiFi + LWIP sdkconfig keys are byte-identical** to
  `c5_hybrid_pcap/sdkconfig.c5min`.
- Across the whole ~4000-key sdkconfig the **only** difference is the partition
  CSV *filename* (`partitions.csv` -> `idf_softap_partitions.csv`; decoded
  contents identical, 2 MiB factory at 0x10000).
- `# CONFIG_SPIRAM is not set`; zero `CONFIG_ARDUINO*` keys; zero Arduino WiFi
  symbols in the link map — no Arduino WiFi anywhere on the association path.

**Arduino envs still green:** `T-Dongle-C5` and `T-Dongle-S3` both rebuild
SUCCESS after the change.

### DONE: main-tree `T-Dongle-C5-idf` flashed + SoftAP regress PASS (2026-11-05)
Flashed `firmware/.pio/build/T-Dongle-C5-idf/firmware.bin` (~1.11 MB) to the
T-Dongle-C5. win11+Hama SoftAP regress PASSED (DHCP 192.168.4.6; HTTP 200 on
`/`, `/raw`, `/api/status`, `/api/payloads`; recon scan/enum/capture held;
authenticated pcap GET 200 with libpcap magic/Content-Type; 401/404 as
expected). Device left on this image; restore not needed. See the
"2026-11-05 — main-tree T-Dongle-C5-idf flash + SoftAP regress PASS" section
below.

### Deliberate gaps vs the Arduino build
- No serial console commands (`SCAN`, `PMF`, `PMFDOWN`, `ENUM`, `DUMP`,
  `TXSTATS`, `LWIPSTATS`) — those are Arduino `src/console/` features. This
  build's control surface is the REST API + the LCD.
- AP-down PMF sweep still stubbed `false` (SoftAP teardown/restore is the
  historically hang-prone path).
- HID HAL is a no-op by design: the C5 has no USB-OTG.

## Strategy (historical, now complete)
HYBRID rebuild on clean from-source ESP-IDF SoftAP (no arduino-esp32 WiFi),
adding feature layers and retesting win11+Hama association after each flash.
That sequence finished at the pcap layer and has now been folded into the repo.

## Device / flash state (2026-11-05)
- **On device now:** main-tree **`T-Dongle-C5-idf`** — FLASHED + **SoftAP regress PASS**
  (win11+Hama: DHCP 192.168.4.6; HTTP/recon/pcap as above).
- **Git:** fix commit `cf8dee6` merged via **PR #28** → `main` `6fae5bc` (pushed).
- **Arduino repro branch:** `docs/arduino-c5-softap-repro` @ `8181303` (pushed; not on `main` yet).
- Prior known-good scratch: `/home/op/scratch/c5_hybrid_pcap` (and earlier layers);
  restore not needed after the main-tree PASS.

## Layer paths
| Layer | Path | Status |
|-------|------|--------|
| restore min | /home/op/scratch/c5_softap_min | PASS SoftAP baseline |
| l2 | /home/op/scratch/c5_hybrid_l2 | PASS (C2 HTTP) |
| l3a | /home/op/scratch/c5_hybrid_l3a | PASS (SD + payloads) |
| l3b | /home/op/scratch/c5_hybrid_l3b | PASS (interpreter) |
| l3c | /home/op/scratch/c5_hybrid_l3c | PASS HW (display + recon) |
| pcap | /home/op/scratch/c5_hybrid_pcap | **PASS HW** (pcap download) |

## pcap layer HW result (2026-11-05)
- Flash: `pio run -e c5min -t upload` SUCCESS; firmware.bin 1,106,928 B.
- win11+Hama: Lily DHCP 192.168.4.3; SoftAP solid through capture+download.
- POST /api/recon/start → recon_67329.pcap; stop → 74 packets / 0 dropped.
- GET /api/recon/pcap/recon_67329.pcap (auth) → **200**, Content-Type
  application/vnd.tcpdump.pcap, 25051 bytes, magic d4:c3:b2:a1.
- No auth → 401; missing name → 404. ping 4/4 after download.
- Restore of l3c not required.

## NEXT (after CERT.PL pause)

1. **DONE PASS (2026-11-05):** Flash merged `T-Dongle-C5-idf` + win11+Hama SoftAP
   regress. Device left on that image. Issue #9 formally **RESOLVED** / closed in KB.
2. **Optional docs:** merge branch `docs/arduino-c5-softap-repro` (`8181303`) into
   `main` (Arduino SoftAP kept as #9 repro — already decided; PR merge still pending).
3. **Optional HW:** visually confirm LCD GREENTAB 160×80 paint on the physical dongle.
4. **Optional / not started:** AP-down **PMFDOWN** on the IDF SoftAP path (still stubbed
   `false` — only if SoftAP teardown/restore is acceptable).
5. **Roadmap (not started; paused for CERT.PL BB):** Phase 3 **BLE HID** (C5's only
   keystroke path); further hybrid beyond pcap if needed.
6. **DECIDED:** Arduino `T-Dongle-C5` SoftAP stays enabled as known-bad #9 repro /
   A/B vs `T-Dongle-C5-idf`. Lab SoftAP / real C2 uses **only** the IDF env.


## Tooling / hygiene
- Reach guest only via laptop vm-gx as user op. Never SSH into arch-pentest.
  Never touch nftables/SSH/VPN. Ask Vee to power VMs when Kasper wants them up.
- cursor-agent always in a visible herdr tab with script/PTY (not buffered
  tee-only); log under /home/op/scratch/*.agent.log.
- Never long-run scripts/serial_monitor.py. Prefer probe_reset.py / short captures.
- Check free -h before heavy builds. Notes in English; talk to Kasper in Polish.
- No git commit/push unless asked; PRs draft by default.

## 2026-11-05 — main-tree T-Dongle-C5-idf flash + SoftAP regress PASS
- Flashed existing build firmware/.pio/build/T-Dongle-C5-idf/firmware.bin (1,106,944 B) to T-Dongle-C5 on /dev/ttyACM0 via pio run -e T-Dongle-C5-idf -t upload (arch-pentest as op).
- Boot: SoftAP UP ssid=Lily ch=6 ip=192.168.4.1, HTTPD :80, C2 16 handlers, SD mounted; no Guru Meditation.
- win11+Hama (RTL8188EU): associated, DHCP 192.168.4.6; GET /, /raw, /api/status, /api/payloads → 200; ping OK.
- Recon scan + enum/0 held SoftAP; capture start/stop held SoftAP; pcap GET with auth 200 (application/vnd.tcpdump.pcap, magic D4C3B2A1), without auth 401, missing 404.
- Device left on main-tree T-Dongle-C5-idf image. No restore. Issue #9 formally closed in KB + committed with this merge.
- Timestamp: 2026-11-05 10:46 Europe/Warsaw (UTC+1)
