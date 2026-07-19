/* yajir_storage.cpp - Nano R4 data-flash autorun storage */
#include "yajir_build_config.h"
#include "script_config.h"
#include <Arduino.h>
#include <DataFlashBlockDevice.h>
#include <stdint.h>
#include <string.h>

#include "yajir_storage.h"

enum {
    STORAGE_MAGIC = 0x524A4159u, /* "YAJR" in little-endian memory */
    STORAGE_FORMAT = 1u,
    STORAGE_HEADER_BLOCKS = 1u,
    STORAGE_SOURCE_BLOCKS = 4u
};

struct storage_header_t {
    uint32_t magic;
    uint32_t format;
    uint32_t length;
    uint32_t crc32;
};

static_assert(sizeof(storage_header_t) == 16u, "unexpected storage header size");

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    while (length-- > 0u) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return crc;
}

static size_t storage_block_size(DataFlashBlockDevice &flash)
{
    return (size_t)flash.get_erase_size();
}

static size_t storage_source_address(DataFlashBlockDevice &flash)
{
    return STORAGE_HEADER_BLOCKS * storage_block_size(flash);
}

static size_t storage_source_capacity(DataFlashBlockDevice &flash)
{
    return STORAGE_SOURCE_BLOCKS * storage_block_size(flash);
}

static size_t storage_reserved_size(DataFlashBlockDevice &flash)
{
    return (STORAGE_HEADER_BLOCKS + STORAGE_SOURCE_BLOCKS) *
           storage_block_size(flash);
}

static int storage_layout_valid(DataFlashBlockDevice &flash)
{
    size_t block_size = storage_block_size(flash);
    return block_size > 0u &&
           storage_source_capacity(flash) >= YAJIR_SOURCE_BUFFER_SIZE &&
           storage_reserved_size(flash) <= (size_t)flash.size();
}

static int verify_payload(DataFlashBlockDevice &flash, size_t address,
                          size_t length, uint32_t expected_crc)
{
    uint8_t buffer[64];
    uint32_t crc = 0xffffffffu;

    while (length > 0u) {
        size_t chunk = length < sizeof(buffer) ? length : sizeof(buffer);
        if (flash.read(buffer, address, chunk) != BLOCK_DEVICE_OK) return 0;
        crc = crc32_update(crc, buffer, chunk);
        address += chunk;
        length -= chunk;
    }
    return (crc ^ 0xffffffffu) == expected_crc;
}

int yajir_storage_load(char *destination, size_t capacity, size_t *length)
{
    DataFlashBlockDevice &flash = DataFlashBlockDevice::getInstance();
    storage_header_t header;
    uint32_t crc;

    if (length) *length = 0u;
    if (!destination || capacity == 0u || !length || !storage_layout_valid(flash))
        return YAJIR_STORAGE_ERROR;
    if (flash.read(&header, 0u, sizeof(header)) != BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;
    if (header.magic == 0xffffffffu) return YAJIR_STORAGE_EMPTY;
    if (header.magic != STORAGE_MAGIC || header.format != STORAGE_FORMAT ||
        header.length == 0u || header.length >= capacity ||
        header.length > storage_source_capacity(flash))
        return YAJIR_STORAGE_CORRUPT;
    if (flash.read(destination, storage_source_address(flash), header.length) !=
        BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;

    crc = crc32_update(0xffffffffu, (const uint8_t *)destination,
                       (size_t)header.length) ^ 0xffffffffu;
    if (crc != header.crc32) return YAJIR_STORAGE_CORRUPT;

    destination[header.length] = '\0';
    *length = (size_t)header.length;
    return YAJIR_STORAGE_OK;
}

int yajir_storage_save(const char *source, size_t length)
{
    DataFlashBlockDevice &flash = DataFlashBlockDevice::getInstance();
    storage_header_t header;
    storage_header_t check;
    size_t source_address;

    if (!source || length == 0u || !storage_layout_valid(flash) ||
        length >= YAJIR_SOURCE_BUFFER_SIZE ||
        length > storage_source_capacity(flash))
        return YAJIR_STORAGE_ERROR;

    source_address = storage_source_address(flash);
    header.magic = STORAGE_MAGIC;
    header.format = STORAGE_FORMAT;
    header.length = (uint32_t)length;
    header.crc32 = crc32_update(0xffffffffu, (const uint8_t *)source, length) ^
                   0xffffffffu;

    if (flash.erase(0u, storage_reserved_size(flash)) != BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;
    if (flash.program(source, source_address, length) != BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;
    if (!verify_payload(flash, source_address, length, header.crc32))
        return YAJIR_STORAGE_ERROR;

    /* Keep magic erased until every fallible payload/header operation passes. */
    if (flash.program((const uint8_t *)&header + sizeof(header.magic),
                      sizeof(header.magic),
                      sizeof(header) - sizeof(header.magic)) != BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;
    if (flash.program(&header.magic, 0u, sizeof(header.magic)) != BLOCK_DEVICE_OK)
        return YAJIR_STORAGE_ERROR;
    if (flash.read(&check, 0u, sizeof(check)) != BLOCK_DEVICE_OK ||
        memcmp(&check, &header, sizeof(header)) != 0)
        return YAJIR_STORAGE_ERROR;
    return YAJIR_STORAGE_OK;
}

int yajir_storage_erase(void)
{
    DataFlashBlockDevice &flash = DataFlashBlockDevice::getInstance();

    if (!storage_layout_valid(flash)) return YAJIR_STORAGE_ERROR;
    return flash.erase(0u, storage_reserved_size(flash)) == BLOCK_DEVICE_OK ?
           YAJIR_STORAGE_OK : YAJIR_STORAGE_ERROR;
}
