// =============================================================================
// board_spi.c - shared SPI2_HOST ownership: bus, mutex, APA102 latch
// =============================================================================
// See board_spi.h for the wiring rationale. Summary of the arbitration model:
//
//   * One bus (SPI2_HOST), initialised exactly once, never freed.
//   * One recursive mutex around device-level *sequences* (a whole LCD
//     CASET/RASET/RAMWR+data run, a whole PCAP frame write, a whole LED latch).
//     IDF already serialises individual transactions per host; the mutex is
//     there to keep a sequence from being split by another device's transfer.
//   * Lock order is always: our mutex -> IDF's internal spi_bus_lock. Nothing
//     in this tree takes them the other way round, so no inversion is possible.
// =============================================================================
#include "board_spi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

static const char *TAG = "board_spi";

static bool s_busUp;
static SemaphoreHandle_t s_busMutex;

// APA102 device. It has no chip select, so it is added with spics_io_num = -1:
// IDF leaves every other device's CS line alone, which is exactly what we need
// (LCD CS GPIO10 and SD CS GPIO23 stay deasserted/high while the LED frame is
// clocked out).
static spi_device_handle_t s_ledDev;

// Cached colour. Sentinel (1,1,1) is not a colour we ever set, so the first
// board_led_set(0,0,0) still goes out.
static uint8_t s_ledR = 1, s_ledG = 1, s_ledB = 1;

esp_err_t board_spi_bus_init(void)
{
    if (s_busUp) return ESP_OK;

    // Mutex first: a failure here is fatal for arbitration, so don't leave the
    // bus half-initialised.
    if (!s_busMutex) {
        s_busMutex = xSemaphoreCreateRecursiveMutex();
        if (!s_busMutex) {
            ESP_LOGE(TAG, "bus mutex alloc failed");
            return ESP_ERR_NO_MEM;
        }
    }

    spi_bus_config_t cfg = {
        .mosi_io_num = BOARD_PIN_MOSI,
        .miso_io_num = BOARD_PIN_MISO,
        .sclk_io_num = BOARD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BOARD_SPI_MAX_TRANSFER,
    };

    esp_err_t err = spi_bus_initialize(BOARD_SPI_HOST, &cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize(SPI2) failed: %s", esp_err_to_name(err));
        return err;
    }
    // ESP_ERR_INVALID_STATE means someone else already brought the host up
    // (an earlier build of this module, or a driver that pre-dates the shared
    // helper). Either way the bus exists and is ours to share.
    s_busUp = true;
    ESP_LOGI(TAG, "SPI2 shared bus up (sclk=%d mosi=%d miso=%d)",
             BOARD_PIN_SCLK, BOARD_PIN_MOSI, BOARD_PIN_MISO);

    // APA102 rides the same MOSI/SCK with no CS. A failure here is not fatal -
    // the LED just stays dark and relatch() reports false - so the LCD and SD
    // keep working.
    spi_device_interface_config_t led = {
        .mode = 0,
        .clock_speed_hz = BOARD_LED_SPI_HZ,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    err = board_spi_add_device(&led, &s_ledDev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APA102 device add failed (%s), LED disabled",
                 esp_err_to_name(err));
        s_ledDev = NULL;
    }
    return ESP_OK;
}

bool board_spi_bus_ready(void) { return s_busUp; }

spi_host_device_t board_spi_host(void) { return BOARD_SPI_HOST; }

esp_err_t board_spi_add_device(const spi_device_interface_config_t *cfg,
                               spi_device_handle_t *out)
{
    if (!s_busUp) return ESP_ERR_INVALID_STATE;
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    return spi_bus_add_device(BOARD_SPI_HOST, cfg, out);
}

void board_spi_lock(void)
{
    if (s_busMutex) xSemaphoreTakeRecursive(s_busMutex, portMAX_DELAY);
}

void board_spi_unlock(void)
{
    if (s_busMutex) xSemaphoreGiveRecursive(s_busMutex);
}

// ── APA102 ──────────────────────────────────────────────────────────────────
// Frame: 32 zero start bits, one LED frame (0xE0|brightness, B, G, R), then 32
// zero end bits. Zeros for the end frame are the portable choice: the APA102
// datasheet's 0xFFFFFFFF is indistinguishable from a valid max-white LED frame
// and can corrupt the last LED on clones, while SK9822 clones *require* the
// zero reset frame. Colour order is BGR (see board_pins.h).
//
// Padded to 12 bytes total so one transfer carries start+LED+end in a single
// CS-less burst, exactly like lily's sendAPA102().
static void apa102_send(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ledDev) return;

    uint8_t buf[12] = {0};
    buf[4] = (uint8_t)(0xE0 | (BOARD_APA102_BRIGHTNESS & 0x1F));
    buf[5] = b;
    buf[6] = g;
    buf[7] = r;
    // buf[8..11] left zero: the end/reset frame.
    //
    // bytes 0..3 are the 32-bit start frame (all zero).

    spi_transaction_t t = {
        .length = sizeof(buf) * 8,
        .tx_buffer = buf,
    };
    esp_err_t err = spi_device_transmit(s_ledDev, &t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "APA102 transmit failed: %s", esp_err_to_name(err));
    }
}

void board_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    // Change-cached like lily's Hal::ledSet(): the dashboard calls this once a
    // second and the colour rarely changes, so the common case costs nothing.
    // board_led_relatch() deliberately bypasses this cache.
    if (r == s_ledR && g == s_ledG && b == s_ledB) return;

    s_ledR = r;
    s_ledG = g;
    s_ledB = b;
    if (!s_busUp || !s_ledDev) return;

    board_spi_lock();
    apa102_send(r, g, b);
    board_spi_unlock();
}

bool board_led_relatch(void)
{
    if (!s_busUp || !s_ledDev) return false;
    // Bypasses the change cache on purpose: the point of a relatch is that the
    // LED is *currently* showing something else (whatever the LCD or SD just
    // clocked through it), so "colour unchanged" is not a reason to skip.
    board_spi_lock();
    apa102_send(s_ledR, s_ledG, s_ledB);
    board_spi_unlock();
    return true;
}

void board_led_status_idle(void)     { board_led_set(80, 80, 0); }
void board_led_status_running(void)  { board_led_set(0, 255, 0); }
void board_led_status_complete(void) { board_led_set(0, 255, 0); }
void board_led_status_error(void)    { board_led_set(255, 0, 0); }
void board_led_status_wifi(void)     { board_led_set(0, 120, 255); }
