// =============================================================================
// storage.h / storage.cpp — SD card payload & loot management
// =============================================================================
// Transport-agnostic: SDMMC 1-bit on the T-Dongle-S3, SPI on the
// T-Dongle-C5. Callers use the Storage API (or Storage::fs() when they
// need a raw File handle) and never touch the SD library directly.
// =============================================================================
#ifndef LILY_DUCKY_STORAGE_H
#define LILY_DUCKY_STORAGE_H

#include <Arduino.h>
#include <FS.h>
#include "config.h"

namespace Storage {

    // Initialise the SD card (transport picked by target in config.h)
    bool init();
    void end();
    bool ready();

    // Card metadata (0 when unmounted)
    uint64_t cardSize();

    // The mounted filesystem. For code that needs File handles directly
    // (e.g. streaming an HTTP upload to disk).
    fs::FS& fs();

    // Payload management
    bool payloadExists(const char* name);
    bool loadPayload(const char* name, char** buf, int* len);
    bool listPayloads(String* out, int maxLen);

    // Loot (exfiltration)
    bool appendLoot(const uint8_t* data, int len);
    bool clearLoot();
    bool lootExists();

    // Filesystem helpers
    bool writeFile(const char* path, const char* content);
    bool writeFile(const char* path, const uint8_t* data, int len);
    bool readFile(const char* path, char** buf, int* len);
    bool deleteFile(const char* path);
    bool dirExists(const char* path);
    bool createDir(const char* path);

} // namespace Storage

#endif
