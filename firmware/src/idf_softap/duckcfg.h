// =============================================================================
// duckcfg.h - local compile-time constants for the DuckyScript interpreter
// =============================================================================
// Only the CFG_* values the interpreter actually uses are copied here from
// lily_ducky's firmware/src/config.h. The Arduino config.h itself is NOT
// imported: it drags in pinout/capability macros for a board family this tree
// does not have, and the C5 build wants an honest, minimal set.
//
// Values are deliberately identical to lily's so payload behaviour does not
// drift. Board differences (no USB HID on the C5) are handled in the HAL stub,
// not here.
// =============================================================================
#ifndef C5_DUCK_CFG_H
#define C5_DUCK_CFG_H

// ── Interpreter tuning (lily: CFG_DEFAULT_DELAY .. CFG_MAX_NESTED) ──────────
#define DUCK_DEFAULT_DELAY      200     // ms between commands (pre-Ducky v2)
#define DUCK_MIN_DELAY          10      // absolute minimum DELAY value
#define DUCK_MAX_VAR            65535   // uint16 cap for DuckvScript variables
#define DUCK_MAX_FUNCTIONS      32      // max named functions in symbol table
#define DUCK_MAX_VAR_COUNT      32      // max $variables at once
#define DUCK_MAX_NESTED         16      // max nested block depth (IF/WHILE/FUNC)

// ── OS detection (lily: CFG_OS_DETECT_WINDOW_MS) ────────────────────────────
// lily waits 3 s for host SET_REPORT traffic. The C5 has no USB-OTG
// peripheral, so there is nothing to observe and no report can ever arrive:
// the window is 0 and DETECT_OS resolves immediately as OS_UNKNOWN instead of
// stalling the script for 3 s. The cooperative state machine in execLine() is
// unchanged, so re-enabling this on an S3-class port is a one-line change.
#define DUCK_OS_DETECT_WINDOW_MS 0

// ── OS ids used by $_OS (lily: OS_UNKNOWN .. OS_CHROMEOS) ───────────────────
#define DUCK_OS_UNKNOWN   0
#define DUCK_OS_WINDOWS   1
#define DUCK_OS_MACOS     2
#define DUCK_OS_LINUX     3
#define DUCK_OS_CHROMEOS  4

// ── Keyboard layouts (lily: LAYOUT_US .. LAYOUT_COUNT) ──────────────────────
#define DUCK_LAYOUT_US  0
#define DUCK_LAYOUT_PL  1
#define DUCK_LAYOUT_DE  2

// ── Parser limits (lily: interpreter.h MAX_LINES / MAX_LINE_LEN) ────────────
// Same caps, but the line table is heap-allocated to fit the payload instead of
// reserving 512*256 = 128 KiB of .bss. The C5 has no PSRAM and only ~384 KiB of
// internal SRAM, so a fixed table that large would starve WiFi + httpd. A
// payload bigger than the caps is rejected at load time (runPayload -> false).
#define DUCK_MAX_LINES      512
#define DUCK_MAX_LINE_LEN   256

// ── Symbol table entry sizes (lily: interpreter.h) ──────────────────────────
#define DUCK_MAX_DEFINES     32
#define DUCK_MAX_FUNC_NAME   32
#define DUCK_MAX_VAR_NAME    16
#define DUCK_MAX_DEFINE_NAME 16

// ── HID modifier masks (lily: interpreter.h HK_* and hal.h KEY_MOD_*) ───────
// Only the modifier mask handed to the (no-op) HAL. Scancodes are unchanged
// from the USB HID usage table so a future HID-capable port needs no edits.
#define DUCK_HK_NONE       0x00
#define DUCK_MOD_LCTRL     0x01
#define DUCK_MOD_LSHIFT    0x02
#define DUCK_MOD_LALT      0x04
#define DUCK_MOD_LGUI      0x08
#define DUCK_MOD_RCTRL     0x10
#define DUCK_MOD_RSHIFT    0x20
#define DUCK_MOD_RALT      0x40
#define DUCK_MOD_RGUI      0x80

#endif  // C5_DUCK_CFG_H
