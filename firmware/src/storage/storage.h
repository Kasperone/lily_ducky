// =============================================================================
// storage.h / storage.cpp — SD card payload & loot management
// =============================================================================
#ifndef FUNNY_USB_STORAGE_H
#define FUNNY_USB_STORAGE_H

#include <Arduino.h>
#include "SD_MMC.h"
#include "config.h"

namespace Storage {

    // Initialise SD card (SDMMC 1-bit mode for T-Dongle-S3)
    bool init();
    void end();
    bool ready();

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
