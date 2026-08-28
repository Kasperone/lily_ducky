# TFT_eSPI — vendored fork (Bodmer/TFT_eSPI 2.5.43)

Vendored from the PlatformIO registry copy of Bodmer/TFT_eSPI 2.5.43 with ONE
change: ESP32-C5 processor support, which upstream does not have (not in
2.5.43, not in master — Bodmer/TFT_eSPI#3751, closed without upstream support).

## Local changes (do not overwrite on re-vendoring)

- `Processors/TFT_eSPI_ESP32_C5.c` / `.h` — NEW. Derived from the ESP32-C3
  files: the C3 and C5 share the single GPSPI2 peripheral and the IDF5
  register layout (`SPI_MS_DLEN_REG`, `SPI_UPDATE` double-buffer handshake,
  union-style GPIO registers). This is the fix validated on real C5 hardware
  in Bodmer/TFT_eSPI#3751 and reused by other C5 firmwares (Bruce, RockBase).
- `TFT_eSPI.h` / `TFT_eSPI.cpp` — one added `#elif` each, routing
  `CONFIG_IDF_TARGET_ESP32C5` to the new files.
- `examples/`, `Tools/`, `User_Setups/` stripped (~31 MB of non-build weight).

Everything else is byte-identical to upstream 2.5.43. If upstream ever ships
C5 support, compare and drop this fork back to `lib_deps`.

## Why not `lib_deps`

PlatformIO re-downloads registry libs per clone into `.pio/libdeps/` (which is
gitignored), so a patched library cannot live there. `firmware/lib/` ships with
the repo — same precedent as the vendored board JSONs in `firmware/boards/`.
