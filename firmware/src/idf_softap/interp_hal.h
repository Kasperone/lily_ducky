// =============================================================================
// interp_hal.h - keyboard HAL stub for the C5 DuckyScript interpreter
// =============================================================================
// The T-Dongle-C5 (ESP32-C5) has NO USB-OTG peripheral: the USB-A port is a
// fixed-function Serial/JTAG console and the chip cannot enumerate as a
// keyboard. Every function here is therefore an honest no-op (debug logging
// only), exactly like lily_ducky's Hal::press()/Hal::release() on the C5
// target (CFG_HAS_USB_HID=0).
//
// This is deliberately NOT ported from hal.cpp: no LED, no LCD, no button, no
// TinyUSB, no MSC. Only the four entry points the interpreter calls exist, so
// nothing on the C5 silently pretends to type.
// =============================================================================
#ifndef C5_DUCK_INTERP_HAL_H
#define C5_DUCK_INTERP_HAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Press a key with modifier mask, then (separately) release everything.
// No-ops on the C5; kept as separate calls so the interpreter's control flow
// stays byte-for-byte identical to lily's.
void duckhal_press(uint8_t mods, uint8_t key);
void duckhal_release(void);

// OS detection. Nothing can be observed without HID, so the result is always
// "unknown" and the SET_REPORT count always 0.
uint8_t duckhal_os_detect_result(void);
uint8_t duckhal_os_detect_count(void);

// ATTACKMODE STORAGE / MSC: the SD card is mounted by storage.c as a normal
// filesystem, never exposed over USB. Always refuses.
bool duckhal_msc_enable(void);

// EXFIL capture: recording our own keystrokes is meaningless when no keystroke
// is ever emitted. No-ops.
void duckhal_capture_begin(void);
void duckhal_capture_end(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_DUCK_INTERP_HAL_H
