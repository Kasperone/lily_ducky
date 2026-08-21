// =============================================================================
// storage.cpp — SD card payload & loot management
// =============================================================================

#include "storage.h"
#include <SD_MMC.h>
#include <FS.h>

bool Storage::init()
{
    // SD_MMC 1-bit mode needs INPUT_PULLUP on all three pins before begin();
    // pin assignments come from config.h.
    pinMode(PIN_SDMMC_CMD, INPUT_PULLUP);
    pinMode(PIN_SDMMC_CLK, INPUT_PULLUP);
    pinMode(PIN_SDMMC_D0,  INPUT_PULLUP);

#if SDMMC_USE_1BIT
    if (!SD_MMC.begin("/sdcard", true)) {  // 1-bit mode
        Serial.println("[SD] 1-bit init failed");
        return false;
    }
#else
    if (!SD_MMC.begin("/sdcard", false)) { // 4-bit mode
        Serial.println("[SD] 4-bit init failed");
        return false;
    }
#endif

    Serial.printf("[SD] OK: %s (%.1f MB)\n",
                  SD_MMC.cardType() == CARD_MMC ? "MMC" : "SD",
                  (float)SD_MMC.cardSize() / 1048576);
    return true;
}

void Storage::end()
{
    SD_MMC.end();
}

bool Storage::ready()
{
    return SD_MMC.cardType() != CARD_NONE;
}

bool Storage::payloadExists(const char* name)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name);
    File f = SD_MMC.open(path);
    bool exists = f && !f.isDirectory();
    if (f) f.close();
    return exists;
}

bool Storage::loadPayload(const char* name, char** buf, int* len)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name);
    File f = SD_MMC.open(path);
    if (!f) return false;
    *len = f.size();
    *buf = (char*)malloc(*len + 1);
    if (!*buf) { f.close(); return false; }
    f.readBytes(*buf, *len);
    (*buf)[*len] = '\0';
    f.close();
    return true;
}

bool Storage::listPayloads(String* out, int maxLen)
{
    File dir = SD_MMC.open(SD_PAYLOAD_DIR);
    if (!dir) return false;
    out->concat("");
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            if (out->length() + entry.name().length() + 2 < (size_t)maxLen) {
                out->concat(entry.name());
                out->concat(",");
            }
        }
        entry = dir.openNextFile();
    }
    entry.close();
    dir.close();
    return true;
}

bool Storage::appendLoot(const uint8_t* data, int len)
{
    File f = SD_MMC.open(SD_LOOT_FILE, FILE_APPEND);
    if (!f) return false;
    f.write(data, len);
    f.close();
    return true;
}

bool Storage::clearLoot()
{
    return deleteFile(SD_LOOT_FILE);
}

bool Storage::lootExists()
{
    File f = SD_MMC.open(SD_LOOT_FILE);
    bool exists = f && !f.isDirectory();
    if (f) f.close();
    return exists;
}

bool Storage::writeFile(const char* path, const char* content)
{
    return writeFile(path, (const uint8_t*)content, strlen(content));
}

bool Storage::writeFile(const char* path, const uint8_t* data, int len)
{
    File f = SD_MMC.open(path, FILE_WRITE);
    if (!f) return false;
    f.write(data, len);
    f.close();
    return true;
}

bool Storage::readFile(const char* path, char** buf, int* len)
{
    File f = SD_MMC.open(path);
    if (!f || f.isDirectory()) return false;
    *len = f.size();
    *buf = (char*)malloc(*len + 1);
    if (!*buf) { f.close(); return false; }
    f.readBytes(*buf, *len);
    (*buf)[*len] = '\0';
    f.close();
    return true;
}

bool Storage::deleteFile(const char* path)
{
    return SD_MMC.remove(path);
}

bool Storage::dirExists(const char* path)
{
    File f = SD_MMC.open(path);
    bool exists = f && f.isDirectory();
    if (f) f.close();
    return exists;
}

bool Storage::createDir(const char* path)
{
    return SD_MMC.mkdir(path);
}
