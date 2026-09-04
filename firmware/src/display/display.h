// =============================================================================
// display/display.h — RGB LED + ST7735 LCD status display
// =============================================================================
// Provides semantic status through colour patterns on the APA102, and —
// when LCD_ENABLED=1 — a dashboard frame on the 80x160 ST7735 (both
// boards use the identical panel).
// =============================================================================
#ifndef LILY_DUCKY_DISPLAY_H
#define LILY_DUCKY_DISPLAY_H

#include "hal/hal.h"
#include "interpreter/interpreter.h"

namespace Display {

    // Call once at start. With LCD_ENABLED, this initialises the ST7735
    // panel and paints the static frame; the LED path runs unconditionally.
    void init();

    // Update display based on current interpreter + WiFi state.
    // Call every loop — paints to the LCD are diff-gated so this stays
    // cheap when nothing has changed.
    void update(InterpState interpState, bool c2Running, int clients,
                bool reconCapturing, uint32_t reconPackets);

} // namespace Display

#endif
