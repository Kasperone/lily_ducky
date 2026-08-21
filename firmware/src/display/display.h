// =============================================================================
// display/display.h — RGB LED status display (LCD-ready for when added later)
// =============================================================================
// Provides semantic status through colour patterns.
// LCD_ENABLED can be flipped to 1 in config.h when we add the screen.
// =============================================================================
#ifndef FUNNY_USB_DISPLAY_H
#define FUNNY_USB_DISPLAY_H

#include "hal/hal.h"
#include "interpreter/interpreter.h"

namespace Display {

    // Call once at start. With LCD_ENABLED, this initialises the ST7735
    // panel and paints the static frame; the LED path runs unconditionally.
    void init();

    // Update display based on current interpreter + WiFi state.
    // Call every loop — paints to the LCD are diff-gated so this stays
    // cheap when nothing has changed.
    void update(InterpState interpState, bool c2Running, int clients);

} // namespace Display

#endif
