/* yajir_sd.cpp - Cardputer microSD autorun and import support */
#include "yajir_build_config.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <stdio.h>

#include "yajir_config_cardputer.h"
#include "yajir_sd.h"

enum {
    SD_SPI_SCK_PIN = 40,
    SD_SPI_MISO_PIN = 39,
    SD_SPI_MOSI_PIN = 14,
    SD_SPI_CS_PIN = 12,
    SD_SPI_FREQUENCY = 25000000
};

static int s_ready;
static int32_t s_size_mb;
static char s_import_buffer[YAJIR_IMPORT_BUFFER_SIZE];

static int load_file(const char *path, char *buffer, size_t capacity,
                     size_t *length)
{
    File file;
    size_t size;
    size_t received;

    if (length) *length = 0;
    if (!s_ready || !path || !buffer || capacity < 2 || !length)
        return YAJIR_SD_ERROR;

    file = SD.open(path, FILE_READ);
    if (!file) return YAJIR_SD_NOT_FOUND;
    if (file.isDirectory()) {
        file.close();
        return YAJIR_SD_NOT_FOUND;
    }

    size = (size_t)file.size();
    if (size >= capacity) {
        file.close();
        return YAJIR_SD_TOO_LARGE;
    }

    received = file.read((uint8_t *)buffer, size);
    file.close();
    if (received != size) return YAJIR_SD_ERROR;

    buffer[received] = '\0';
    *length = received;
    return YAJIR_SD_FOUND;
}

static int library_name_valid(const char *name)
{
    size_t length = 0;

    if (!name || !name[0]) return 0;
    while (*name) {
        uint8_t c = (uint8_t)*name++;

        if (++length > 8u) return 0;
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

static int script_name_valid(const char *name, size_t *base_length)
{
    size_t length;
    size_t base;

    if (!name || !name[0]) return 0;
    length = strlen(name);
    base = length;
    if (length >= 4u && name[length - 4u] == '.' &&
        (name[length - 3u] == 'y' || name[length - 3u] == 'Y') &&
        (name[length - 2u] == 'a' || name[length - 2u] == 'A') &&
        (name[length - 1u] == 'j' || name[length - 1u] == 'J')) {
        base -= 4u;
    } else if (strchr(name, '.') != NULL) {
        return 0;
    }

    if (base == 0u || base > 8u) return 0;
    for (size_t i = 0; i < base; ++i) {
        uint8_t c = (uint8_t)name[i];

        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    if (base_length) *base_length = base;
    return 1;
}

int yajir_sd_init(void)
{
    uint8_t card_type;

    s_ready = 0;
    s_size_mb = 0;
    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN,
              SD_SPI_CS_PIN);
    if (!SD.begin(SD_SPI_CS_PIN, SPI, SD_SPI_FREQUENCY)) return -1;

    card_type = SD.cardType();
    if (card_type == CARD_NONE) {
        SD.end();
        return -1;
    }

    s_size_mb = (int32_t)(SD.cardSize() / (1024u * 1024u));
    s_ready = 1;
    return 0;
}

int32_t yajir_sd_ready(void)
{
    return s_ready;
}

int32_t yajir_sd_size_mb(void)
{
    return s_size_mb;
}

int yajir_sd_load_autorun(char *buffer, size_t capacity, size_t *length)
{
    return load_file("/autorun.yaj", buffer, capacity, length);
}

int yajir_sd_load_script(const char *name, char *buffer, size_t capacity,
                         size_t *length)
{
    char path[16];
    size_t base_length;

    if (!script_name_valid(name, &base_length))
        return YAJIR_SD_INVALID_NAME;
    snprintf(path, sizeof(path), "/%.*s.yaj", (int)base_length, name);
    return load_file(path, buffer, capacity, length);
}

int yajir_sd_import(const char *name, const char **source, uint32_t *length)
{
    char path[32];
    size_t source_length = 0;
    int status;

    if (!source || !length || !library_name_valid(name)) return -1;

    snprintf(path, sizeof(path), "/lib/%s.yaj", name);
    status = load_file(path, s_import_buffer, sizeof(s_import_buffer),
                       &source_length);
    if (status == YAJIR_SD_NOT_FOUND) {
        snprintf(path, sizeof(path), "/%s.yaj", name);
        status = load_file(path, s_import_buffer, sizeof(s_import_buffer),
                           &source_length);
    }
    if (status != YAJIR_SD_FOUND) return -1;

    *source = s_import_buffer;
    *length = (uint32_t)source_length;
    return 0;
}
