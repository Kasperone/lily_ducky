// =============================================================================
// storage.cpp — SD card payload & loot management
// =============================================================================
// One filesystem object (_sd) fronts whichever transport the target wires:
// SD_MMC (1-bit SDMMC) on the T-Dongle-S3, SD-over-SPI on the T-Dongle-C5.
// =============================================================================

#include "storage.h"

#if defined(TARGET_DONGLE_S3)
#include <SD_MMC.h>
static fs::FS& _sd = SD_MMC;
// cardType/cardSize live on the concrete SDMMCFS/SDFS classes, not on
// the fs::FS base — route them through these helpers.
static uint8_t  sdCardType() { return SD_MMC.cardType(); }
static uint64_t sdCardSize() { return SD_MMC.cardSize(); }
#else
#include <SD.h>
#include <SPI.h>
static fs::FS& _sd = SD;
static uint8_t  sdCardType() { return SD.cardType(); }
static uint64_t sdCardSize() { return SD.cardSize(); }
#endif

fs::FS& Storage::fs()
{
    return _sd;
}

bool Storage::init()
{
#if defined(TARGET_DONGLE_S3)
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
#else
    // C5: SD shares the LCD's SPI bus; only CS differs. Begin the bus with
    // the SD pins (LilyGO factory wiring), SD.begin() claims the CS line.
    SPI.begin(PIN_LCD_SCK, PIN_LCD_MISO, PIN_LCD_MOSI, PIN_SD_CS);
    if (!SD.begin(PIN_SD_CS)) {
        Serial.println("[SD] SPI init failed");
        return false;
    }
#endif

    Serial.printf("[SD] OK: %s (%.1f MB)\n",
                  sdCardType() == CARD_MMC ? "MMC" : "SD",
                  (float)sdCardSize() / 1048576);
    return true;
}

void Storage::end()
{
#if defined(TARGET_DONGLE_S3)
    SD_MMC.end();
#else
    SD.end();
#endif
}

bool Storage::ready()
{
    return sdCardType() != CARD_NONE;
}

uint64_t Storage::cardSize()
{
    return sdCardSize();
}

bool Storage::payloadExists(const char* name)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name);
    File f = _sd.open(path);
    bool exists = f && !f.isDirectory();
    if (f) f.close();
    return exists;
}

bool Storage::loadPayload(const char* name, char** buf, int* len)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", SD_PAYLOAD_DIR, name);
    File f = _sd.open(path);
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
    File dir = _sd.open(SD_PAYLOAD_DIR);
    if (!dir) return false;
    out->concat("");
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            // File::name() returns const char* in Arduino-ESP32 3.x
            const char* nm = entry.name();
            if (out->length() + strlen(nm) + 2 < (size_t)maxLen) {
                out->concat(nm);
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
    File f = _sd.open(SD_LOOT_FILE, FILE_APPEND);
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
    File f = _sd.open(SD_LOOT_FILE);
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
    File f = _sd.open(path, FILE_WRITE);
    if (!f) return false;
    f.write(data, len);
    f.close();
    return true;
}

bool Storage::readFile(const char* path, char** buf, int* len)
{
    File f = _sd.open(path);
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
    return _sd.remove(path);
}

bool Storage::dirExists(const char* path)
{
    File f = _sd.open(path);
    bool exists = f && f.isDirectory();
    if (f) f.close();
    return exists;
}

bool Storage::createDir(const char* path)
{
    return _sd.mkdir(path);
}
