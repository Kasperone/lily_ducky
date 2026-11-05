// =============================================================================
// interp_hal.c - keyboard HAL stub for the C5 (every call is a no-op)
// =============================================================================
// See interp_hal.h for why. Kept as its own translation unit so that the day a
// HID-capable board is added, only this file changes.
// =============================================================================
#include "interp_hal.h"

#include "esp_log.h"

#include "duckcfg.h"

// Verbose HID tracing (every keypress) is compiled out by default: with no HID
// peripheral it is pure log noise, and the interpreter emits thousands of calls
// for a long STRING. Build with -DDUCK_HAL_TRACE=1 to see them.
#ifndef DUCK_HAL_TRACE
#define DUCK_HAL_TRACE 0
#endif

#if DUCK_HAL_TRACE
#define HAL_LOGD(...) ESP_LOGD("duckhal", __VA_ARGS__)
#else
#define HAL_LOGD(...) do { } while (0)
#endif

void duckhal_press(uint8_t mods, uint8_t key)
{
    HAL_LOGD("press mods=0x%02x key=0x%02x (no USB HID on C5)", mods, key);
    (void)mods;
    (void)key;
}

void duckhal_release(void)
{
    HAL_LOGD("release (no USB HID on C5)");
}

uint8_t duckhal_os_detect_result(void)
{
    // No USB-OTG => no host SET_REPORT traffic to fingerprint.
    return DUCK_OS_UNKNOWN;
}

uint8_t duckhal_os_detect_count(void)
{
    return 0;
}

bool duckhal_msc_enable(void)
{
    // The SD card is a plain FATFS mount here, never a USB mass-storage LUN.
    ESP_LOGD("duckhal", "ATTACKMODE STORAGE refused (no USB on C5)");
    return false;
}

void duckhal_capture_begin(void)
{
    ESP_LOGD("duckhal", "EXFIL_START ignored (no HID keystrokes to capture)");
}

void duckhal_capture_end(void)
{
    ESP_LOGD("duckhal", "EXFIL_STOP ignored (no HID keystrokes to capture)");
}
