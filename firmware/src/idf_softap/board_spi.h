// =============================================================================
// board_spi.h - the one SPI2_HOST bus shared by SD + LCD + APA102 (layer 3c)
// =============================================================================
// The T-Dongle-C5 wires the SD card, the ST7735 LCD and the APA102 status LED
// to the SAME two wires (MOSI=GPIO2, SCLK=GPIO6). SD and LCD each have a chip
// select; the LED has none, so it sees every byte that crosses the bus.
//
// This module owns the bus:
//
//   1. board_spi_bus_init() is idempotent and is the ONLY place that calls
//      spi_bus_initialize()/spi_bus_free() for SPI2_HOST. storage.c (SD) and
//      duck_display.c (LCD) both go through it. Because the bus is never freed
//      on an SD mount failure any more, a missing card no longer takes the LCD
//      down with it (and vice versa).
//   2. board_spi_lock()/board_spi_unlock() is a recursive FreeRTOS mutex. IDF's
//      spi_device transactions already serialise individual transfers per host,
//      but an LCD frame is a *sequence* (CASET, RASET, RAMWR, then N data
//      chunks); without the mutex an SD access could be interleaved between
//      them and both devices would see a corrupt command stream.
//   3. board_led_set()/board_led_relatch() drive the APA102 over that same bus.
//      Re-latching after LCD/SD traffic is mandatory: the LED latches whatever
//      bits clock past it, so a paint leaves it on garbage otherwise.
//
// Lock ordering: the mutex is taken around a whole device-level operation and
// is never taken inside a single spi_device_transmit() callback. SDSPI keeps
// its own internal locking; we do not hold our mutex across a VFS call.
// =============================================================================
#ifndef C5_BOARD_SPI_H
#define C5_BOARD_SPI_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialises SPI2_HOST with the board's shared pins and creates the bus
// mutex. Idempotent: the second and later calls return ESP_OK without
// re-initialising. Must succeed before any device (SD/LCD/LED) is added.
esp_err_t board_spi_bus_init(void);

// True once board_spi_bus_init() has succeeded and the bus is still up.
bool board_spi_bus_ready(void);

// The host id every device on this board belongs to (SPI2_HOST). Exported so
// storage.c can hand it to SDSPI_HOST_DEFAULT() without duplicating the pin
// map.
spi_host_device_t board_spi_host(void);

// Adds a device to the shared bus. Wrapper around spi_bus_add_device() that
// fails cleanly when the bus is not up. *out is only written on success.
esp_err_t board_spi_add_device(const spi_device_interface_config_t *cfg,
                               spi_device_handle_t *out);

// Recursive mutex around a multi-transaction sequence (a whole LCD paint, or
// a burst of LED frames). Never held across a blocking SD VFS call.
void board_spi_lock(void);
void board_spi_unlock(void);

// ── APA102 status LED ───────────────────────────────────────────────────────
// Sets the status colour and latches it. The colour is cached, so
// board_led_relatch() can restore it after LCD/SD traffic clocked garbage
// through the LED. Safe to call before the bus is up (it only caches).
void board_led_set(uint8_t r, uint8_t g, uint8_t b);

// Re-sends the cached colour unconditionally. This is the fix for the shared
// bus: call it after any LCD paint or SD burst. Cheap (12 bytes at 4 MHz).
// Returns false when the bus/LED device is not available yet.
bool board_led_relatch(void);

// Semantic colours, matching lily's Hal::status*() values so the LED reads the
// same as on the reference firmware.
void board_led_status_idle(void);      // amber  (80, 80, 0)
void board_led_status_running(void);   // green  (0, 255, 0)
void board_led_status_complete(void);  // green  (0, 255, 0)
void board_led_status_error(void);     // red    (255, 0, 0)
void board_led_status_wifi(void);      // teal   (0, 120, 255)

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // C5_BOARD_SPI_H
