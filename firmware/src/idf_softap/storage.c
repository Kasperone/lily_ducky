// =============================================================================
// storage.c - SD card storage backend on plain ESP-IDF (layer 3a + 3c)
// =============================================================================
// T-Dongle-C5 SD wiring (SD-over-SPI, shared LCD/SD bus):
//   SCLK = GPIO6, MOSI = GPIO2, MISO = GPIO7, CS = GPIO23, host = SPI2_HOST.
//
// Layer 3c: the SPI bus is no longer owned by this module. board_spi.c owns
// SPI2_HOST because the LCD is a second device on it; storage.c only adds its
// own sdspi device. Two consequences that matter:
//
//   * A mount failure no longer tears the bus down. spi_bus_free() here would
//     leave the LCD without a bus for the rest of the boot, so the bus stays up
//     and the missing card is just reported to the caller.
//   * Payload I/O (a VFS burst of many SPI transactions) holds the board mutex,
//     so an LCD paint cannot be interleaved into the middle of a file write.
//     The APA102 shares MOSI/SCK with no chip select, so the status colour is
//     re-latched once the burst is done.
//
// The card is mounted read-write through esp_vfs_fat_sdspi_mount() at
// /sdcard; payloads live in /sdcard/payloads and recon captures in
// /sdcard/recon. Nothing here aborts on a missing card: the mount just fails, a
// warning is logged, and the hooks keep answering the layer-2 "no storage"
// shape.
// =============================================================================
#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "sdmmc_cmd.h"

#include "board_pins.h"
#include "board_spi.h"

static const char *TAG = "storage";

// ── Board wiring ────────────────────────────────────────────────────────────
// Pins and the host id live in board_pins.h because the LCD shares them.
#define SD_PIN_CLK   BOARD_PIN_SCLK
#define SD_PIN_MOSI  BOARD_PIN_MOSI
#define SD_PIN_MISO  BOARD_PIN_MISO
#define SD_PIN_CS    BOARD_SD_CS

// 20 MHz is the SDSPI ceiling; the driver still negotiates downwards per card.
#define SD_MAX_FREQ_KHZ BOARD_SD_MAX_FREQ_KHZ

// Payload directory under the mount point. Matches C2_SD_PAYLOAD_DIR in
// c2_server.h ("/payloads").
#define PAYLOAD_DIR STORAGE_MOUNT_POINT "/payloads"
// Recon capture directory. Matches C2_SD_RECON_DIR in c2_server.h ("/recon").
// Created on mount (layer 3c requirement 4) so a capture never has to race a
// mkdir, and so GET /api/recon/pcap/<name> has a directory to serve from.
#define RECON_DIR STORAGE_MOUNT_POINT "/recon"

// Directory scan buffer. Names are capped at 48 bytes to match
// C2_MAX_PAYLOAD_FN in c2_server.h: anything longer could be listed but never
// fetched back through /api/payload/<name>, so it is skipped here.
#define STORAGE_MAX_NAME 48
// Upper bound on the cached listing, keeps the RAM cost fixed (~3.5 KB).
#define STORAGE_MAX_FILES 48

typedef struct {
    char   name[STORAGE_MAX_NAME];
    size_t size;
} payload_entry_t;

static sdmmc_card_t *s_card;
static bool s_mounted;

static payload_entry_t s_entries[STORAGE_MAX_FILES];
static int s_entryCount;
static bool s_listingValid;

// ── Mount ───────────────────────────────────────────────────────────────────
esp_err_t storage_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    // The bus is shared with the LCD and the APA102 and is owned by board_spi.c.
    // Bringing it up here (idempotent) means storage works even on a boot where
    // the display was never initialised.
    esp_err_t err = board_spi_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "shared SPI bus unavailable: %s", esp_err_to_name(err));
        return err;
    }

    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.format_if_mount_failed = false;  // never repartition a user card
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board_spi_host();
    host.max_freq_khz = SD_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = board_spi_host();

    err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_card);
    if (err != ESP_OK) {
        // No card inserted is the normal case on a bare board: warn and keep
        // booting. HTTP still works, payload endpoints answer "no storage".
        //
        // The bus is deliberately NOT freed. The LCD lives on the same host and
        // is still expected to paint, so tearing the bus down here would kill
        // the dashboard for the whole boot just because the card slot is empty.
        ESP_LOGW(TAG, "SD mount failed (%s), continuing without card "
                      "(shared SPI bus stays up for the LCD)",
                 esp_err_to_name(err));
        s_card = NULL;
        s_mounted = false;
        return err;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s (shared SPI2 with LCD)", STORAGE_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);

    // Both directories must exist before the C2 hooks can report a usable
    // listing / serve a capture file, so they are created on first mount.
    if (mkdir(PAYLOAD_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", PAYLOAD_DIR, errno);
    }
    if (mkdir(RECON_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", RECON_DIR, errno);
    }
    // A mount is a burst of SPI traffic on the LED's bus.
    board_led_relatch();
    return ESP_OK;
}

bool storage_mounted(void)
{
    return s_mounted;
}

// ── Path building ───────────────────────────────────────────────────────────
// Bare filename -> absolute path. The C2 layer already rejects separators and
// "..", but the check is repeated here so the module is safe on its own.
static bool build_payload_path(const char *name, char *out, size_t outLen)
{
    if (!name || !name[0]) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    if (name[0] == '.') return false;

    int n = snprintf(out, outLen, "%s/%s", PAYLOAD_DIR, name);
    return n > 0 && (size_t)n < outLen;
}

// Same rules as build_payload_path(), but for the recon capture directory.
static bool build_recon_path(const char *name, char *out, size_t outLen)
{
    if (!name || !name[0]) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    if (strstr(name, "..")) return false;
    if (name[0] == '.') return false;

    int n = snprintf(out, outLen, "%s/%s", RECON_DIR, name);
    return n > 0 && (size_t)n < outLen;
}

// ── Directory listing ───────────────────────────────────────────────────────
static int cmp_entry(const void *a, const void *b)
{
    const payload_entry_t *ea = (const payload_entry_t *)a;
    const payload_entry_t *eb = (const payload_entry_t *)b;
    return strcmp(ea->name, eb->name);
}

static void scan_payloads(void)
{
    s_entryCount = 0;
    s_listingValid = false;

    if (!s_mounted) {
        return;
    }

    // Recreate the directory if something removed it; an existing one makes
    // this a cheap no-op.
    if (mkdir(PAYLOAD_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "payload dir unavailable (errno=%d)", errno);
        return;
    }

    DIR *dir = opendir(PAYLOAD_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "opendir %s failed (errno=%d)", PAYLOAD_DIR, errno);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_entryCount < STORAGE_MAX_FILES) {
        if (ent->d_name[0] == '.') continue;  // ".", "..", hidden
        if (strlen(ent->d_name) >= STORAGE_MAX_NAME) continue;

        char path[sizeof(PAYLOAD_DIR) + STORAGE_MAX_NAME + 2];
        if (!build_payload_path(ent->d_name, path, sizeof(path))) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        payload_entry_t *e = &s_entries[s_entryCount++];
        strncpy(e->name, ent->d_name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = '\0';
        e->size = (size_t)st.st_size;
    }
    closedir(dir);

    if (s_entryCount > 1) {
        qsort(s_entries, (size_t)s_entryCount, sizeof(s_entries[0]), cmp_entry);
    }
    s_listingValid = true;
}

bool storage_list_payloads(void)
{
    if (!s_mounted) {
        return false;
    }
    scan_payloads();
    return s_listingValid;
}

int storage_payload_count(void)
{
    // The C2 handler always calls listPayloads() first, but self-heal here so
    // a write that invalidated the cache cannot silently report an empty dir.
    if (s_mounted && !s_listingValid) {
        scan_payloads();
    }
    return s_listingValid ? s_entryCount : 0;
}

const char *storage_payload_name(int idx)
{
    if (!s_listingValid || idx < 0 || idx >= s_entryCount) {
        return NULL;
    }
    return s_entries[idx].name;
}

size_t storage_payload_size(int idx)
{
    if (!s_listingValid || idx < 0 || idx >= s_entryCount) {
        return 0;
    }
    return s_entries[idx].size;
}

// ── File I/O ────────────────────────────────────────────────────────────────
// Every read/write burst holds the board mutex: a payload can be several KiB,
// i.e. many SPI transactions, and the LCD must never be interleaved into the
// middle of one. The LED is re-latched afterwards because it shares MOSI/SCK.
bool storage_read_payload(const char *name, char **outBuf, size_t *outLen)
{
    if (!outBuf || !outLen) return false;
    *outBuf = NULL;
    *outLen = 0;

    if (!s_mounted) return false;

    char path[sizeof(PAYLOAD_DIR) + STORAGE_MAX_NAME + 2];
    if (!build_payload_path(name, path, sizeof(path))) return false;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    size_t len = (size_t)st.st_size;
    // +1 for the NUL the C2 layer expects after the payload bytes.
    char *buf = malloc(len + 1);
    if (!buf) {
        ESP_LOGE(TAG, "out of memory reading %s (%u bytes)", path,
                 (unsigned)len);
        return false;
    }

    board_spi_lock();
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (f) {
        got = len ? fread(buf, 1, len, f) : 0;
        fclose(f);
    }
    board_spi_unlock();
    board_led_relatch();

    if (!got && len) {
        free(buf);
        return false;
    }

    if (got != len) {
        ESP_LOGW(TAG, "short read on %s (%u/%u)", path, (unsigned)got,
                 (unsigned)len);
        free(buf);
        return false;
    }

    buf[len] = '\0';
    *outBuf = buf;
    *outLen = len;
    return true;
}

// Binary-safe read from /sdcard/recon/<name>. Mirrors storage_read_payload()
// but without the NUL terminator: a capture is a libpcap file, so the exact
// byte count from stat() is the contract and strlen() is never applied.
bool storage_read_recon(const char *name, uint8_t **outBuf, size_t *outLen)
{
    if (!outBuf || !outLen) return false;
    *outBuf = NULL;
    *outLen = 0;

    if (!s_mounted) return false;

    char path[sizeof(RECON_DIR) + STORAGE_MAX_NAME + 2];
    if (!build_recon_path(name, path, sizeof(path))) return false;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }

    size_t len = (size_t)st.st_size;
    // No room for a terminator: the buffer holds file bytes only. An empty
    // file still yields a valid (1-byte) allocation so *outBuf != NULL.
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf) {
        ESP_LOGE(TAG, "out of memory reading %s (%u bytes)", path,
                 (unsigned)len);
        return false;
    }

    board_spi_lock();
    FILE *f = fopen(path, "rb");
    size_t got = 0;
    if (f) {
        got = len ? fread(buf, 1, len, f) : 0;
        fclose(f);
    }
    board_spi_unlock();
    board_led_relatch();

    if (!got && len) {
        free(buf);
        return false;
    }

    if (got != len) {
        ESP_LOGW(TAG, "short read on %s (%u/%u)", path, (unsigned)got,
                 (unsigned)len);
        free(buf);
        return false;
    }

    *outBuf = buf;
    *outLen = len;
    return true;
}

bool storage_write_payload(const char *name, const char *body, size_t len)
{
    if (!s_mounted) return false;
    if (len > 0 && !body) return false;

    char path[sizeof(PAYLOAD_DIR) + STORAGE_MAX_NAME + 2];
    if (!build_payload_path(name, path, sizeof(path))) return false;

    if (mkdir(PAYLOAD_DIR, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "payload dir unavailable (errno=%d)", errno);
        return false;
    }

    // Same shared-bus rule as the read path: hold the mutex across the whole
    // write burst, then restore the LED colour.
    board_spi_lock();
    FILE *f = fopen(path, "wb");  // create/truncate
    if (!f) {
        board_spi_unlock();
        ESP_LOGE(TAG, "open %s for write failed (errno=%d)", path, errno);
        return false;
    }

    bool ok = true;
    if (len > 0 && fwrite(body, 1, len, f) != len) {
        ESP_LOGE(TAG, "write %s failed", path);
        ok = false;
    }
    if (fflush(f) != 0) {
        ESP_LOGE(TAG, "flush %s failed", path);
        ok = false;
    }
    if (fclose(f) != 0) {
        ESP_LOGE(TAG, "close %s failed", path);
        ok = false;
    }
    board_spi_unlock();
    board_led_relatch();

    if (!ok) {
        return false;
    }

    // Keep the cached listing in step with what is on the card.
    s_listingValid = false;
    ESP_LOGI(TAG, "wrote %u bytes to %s", (unsigned)len, path);
    return true;
}
