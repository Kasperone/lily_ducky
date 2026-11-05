// =============================================================================
// storage.h - SD card storage backend (layer 3a)
// =============================================================================
// Pure ESP-IDF SD-over-SPI + FATFS/VFS backend for the payload directory.
// Mounts read-write at /sdcard and exposes the index-based payload listing
// that c2_server.c drives through c2_hooks_t.
//
// Everything here is safe to call before/without a mounted card: the listing
// hooks report "unavailable" and the read/write hooks fail, which is exactly
// the shape the layer-2 C2 handlers already fall back to.
// =============================================================================
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MOUNT_POINT "/sdcard"

// Mount the card (once) and make sure the payload directory exists. Returns
// ESP_OK only when the filesystem is mounted and usable. Never aborts: a
// missing card is reported to the caller so the firmware can keep booting.
esp_err_t storage_init(void);

// True once storage_init() succeeded and the card is still mounted.
bool storage_mounted(void);

// ── c2_hooks_t payload surface ─────────────────────────────────────────────
// listPayloads: refreshes the cached directory listing, returns true when the
// payload directory is available (creating it if missing).
bool storage_list_payloads(void);
int storage_payload_count(void);
// NULL when idx is out of range. Valid until the next listPayloads() call.
const char *storage_payload_name(int idx);
size_t storage_payload_size(int idx);

// Reads /sdcard/payloads/<name> into a malloc'd, NUL-terminated buffer.
// On success *outBuf must be freed by the caller and *outLen is the byte
// length (excluding the terminator). False when the file is missing/invalid.
bool storage_read_payload(const char *name, char **outBuf, size_t *outLen);

// Creates/truncates /sdcard/payloads/<name> with len bytes of body.
bool storage_write_payload(const char *name, const char *body, size_t len);

// ── c2_hooks_t recon capture surface ───────────────────────────────────────
// Reads /sdcard/recon/<name> into a malloc'd buffer. Unlike
// storage_read_payload() this is binary-safe: no NUL terminator is appended
// and *outLen is the exact file size, because the bytes are a libpcap file.
// On success *outBuf must be freed by the caller. False when the card is not
// mounted, the name is unsafe, or the file is missing/unreadable.
bool storage_read_recon(const char *name, uint8_t **outBuf, size_t *outLen);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // STORAGE_H
