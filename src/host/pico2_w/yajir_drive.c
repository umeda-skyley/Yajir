/* yajir_drive.c - flash-backed FAT12 MSC and 8.3 script reader */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "pico/flash.h"
#include "pico/platform.h"
#include "tusb.h"

#include "yajir_drive.h"

#define DRIVE_OFFSET       (3840u * 1024u)
#define DRIVE_SIZE         (256u * 1024u)
#define DRIVE_BLOCK_SIZE   512u
#define DRIVE_BLOCK_COUNT  (DRIVE_SIZE / DRIVE_BLOCK_SIZE)
#define DRIVE_CACHE_NONE   UINT32_MAX

#define FAT_START_LBA      1u
#define FAT_SECTORS        2u
#define ROOT_START_LBA     3u
#define ROOT_SECTORS       4u
#define DATA_START_LBA     7u

static uint8_t s_cache[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
static uint32_t s_cache_block = DRIVE_CACHE_NONE;
static bool s_cache_dirty;
static bool s_ready;
static bool s_ejected;

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint8_t const *data;
} flash_operation_t;

static void __not_in_flash_func(commit_sector)(void *context)
{
    flash_operation_t const *operation = context;

    flash_range_erase(operation->offset, FLASH_SECTOR_SIZE);
    flash_range_program(operation->offset, operation->data,
                        FLASH_SECTOR_SIZE);
}

static void __not_in_flash_func(format_partition)(void *context)
{
    flash_operation_t const *operation = context;

    flash_range_erase(operation->offset, operation->size);
    flash_range_program(operation->offset, operation->data,
                        FLASH_SECTOR_SIZE);
}

static uint8_t const *drive_xip(void)
{
    return (uint8_t const *)(XIP_BASE + DRIVE_OFFSET);
}

static int cache_flush(void)
{
    flash_operation_t operation;
    int result;

    if (!s_cache_dirty || s_cache_block == DRIVE_CACHE_NONE) return 0;
    operation.offset = DRIVE_OFFSET + s_cache_block * FLASH_SECTOR_SIZE;
    operation.size = FLASH_SECTOR_SIZE;
    operation.data = s_cache;
    result = flash_safe_execute(commit_sector, &operation, UINT32_MAX);
    if (result == PICO_OK) s_cache_dirty = false;
    return result == PICO_OK ? 0 : -1;
}

static int cache_load(uint32_t block)
{
    if (block == s_cache_block) return 0;
    if (cache_flush() != 0) return -1;
    if (block * FLASH_SECTOR_SIZE >= DRIVE_SIZE) return -1;

    memcpy(s_cache, drive_xip() + block * FLASH_SECTOR_SIZE,
           FLASH_SECTOR_SIZE);
    s_cache_block = block;
    s_cache_dirty = false;
    return 0;
}

static int drive_read(uint32_t offset, void *buffer, size_t length)
{
    uint8_t *output = buffer;

    if (offset > DRIVE_SIZE || length > DRIVE_SIZE - offset) return -1;
    while (length > 0) {
        uint32_t block = offset / FLASH_SECTOR_SIZE;
        uint32_t within = offset % FLASH_SECTOR_SIZE;
        size_t chunk = FLASH_SECTOR_SIZE - within;
        uint8_t const *source;

        if (chunk > length) chunk = length;
        source = block == s_cache_block ? s_cache
                                        : drive_xip() + block * FLASH_SECTOR_SIZE;
        memcpy(output, source + within, chunk);
        output += chunk;
        offset += (uint32_t)chunk;
        length -= chunk;
    }
    return 0;
}

static int drive_write(uint32_t offset, void const *buffer, size_t length)
{
    uint8_t const *input = buffer;

    if (offset > DRIVE_SIZE || length > DRIVE_SIZE - offset) return -1;
    while (length > 0) {
        uint32_t block = offset / FLASH_SECTOR_SIZE;
        uint32_t within = offset % FLASH_SECTOR_SIZE;
        size_t chunk = FLASH_SECTOR_SIZE - within;

        if (chunk > length) chunk = length;
        if (cache_load(block) != 0) return -1;
        memcpy(s_cache + within, input, chunk);
        s_cache_dirty = true;
        input += chunk;
        offset += (uint32_t)chunk;
        length -= chunk;
    }
    return 0;
}

static void put16(uint8_t *data, uint32_t offset, uint16_t value)
{
    data[offset] = (uint8_t)value;
    data[offset + 1u] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *data, uint32_t offset, uint32_t value)
{
    put16(data, offset, (uint16_t)value);
    put16(data, offset + 2u, (uint16_t)(value >> 16));
}

static uint16_t get16(uint8_t const *data, uint32_t offset)
{
    return (uint16_t)(data[offset] | ((uint16_t)data[offset + 1u] << 8));
}

static uint32_t get32(uint8_t const *data, uint32_t offset)
{
    return get16(data, offset) | ((uint32_t)get16(data, offset + 2u) << 16);
}

static bool partition_is_formatted(void)
{
    uint8_t const *boot = drive_xip();

    return get16(boot, 11) == DRIVE_BLOCK_SIZE &&
           get16(boot, 19) == DRIVE_BLOCK_COUNT &&
           boot[510] == 0x55 && boot[511] == 0xAA &&
           memcmp(boot + 43, "YAJIR      ", 11) == 0;
}

static int create_partition(void)
{
    flash_operation_t operation;

    memset(s_cache, 0, sizeof(s_cache));
    s_cache[0] = 0xEB;
    s_cache[1] = 0x3C;
    s_cache[2] = 0x90;
    memcpy(s_cache + 3, "MSDOS5.0", 8);
    put16(s_cache, 11, DRIVE_BLOCK_SIZE);
    s_cache[13] = 1;
    put16(s_cache, 14, 1);
    s_cache[16] = 1;
    put16(s_cache, 17, 64);
    put16(s_cache, 19, DRIVE_BLOCK_COUNT);
    s_cache[21] = 0xF8;
    put16(s_cache, 22, FAT_SECTORS);
    put16(s_cache, 24, 1);
    put16(s_cache, 26, 1);
    s_cache[36] = 0x80;
    s_cache[38] = 0x29;
    put32(s_cache, 39, 0x59414A52u);
    memcpy(s_cache + 43, "YAJIR      ", 11);
    memcpy(s_cache + 54, "FAT12   ", 8);
    s_cache[510] = 0x55;
    s_cache[511] = 0xAA;

    s_cache[FAT_START_LBA * DRIVE_BLOCK_SIZE + 0] = 0xF8;
    s_cache[FAT_START_LBA * DRIVE_BLOCK_SIZE + 1] = 0xFF;
    s_cache[FAT_START_LBA * DRIVE_BLOCK_SIZE + 2] = 0xFF;

    memcpy(s_cache + ROOT_START_LBA * DRIVE_BLOCK_SIZE,
           "YAJIR      ", 11);
    s_cache[ROOT_START_LBA * DRIVE_BLOCK_SIZE + 11] = 0x08;

    operation.offset = DRIVE_OFFSET;
    operation.size = DRIVE_SIZE;
    operation.data = s_cache;
    if (flash_safe_execute(format_partition, &operation, UINT32_MAX) != PICO_OK)
        return -1;

    s_cache_block = DRIVE_CACHE_NONE;
    s_cache_dirty = false;
    return 0;
}

int yajir_drive_init(void)
{
    s_cache_block = DRIVE_CACHE_NONE;
    s_cache_dirty = false;
    s_ejected = false;
    s_ready = partition_is_formatted() || create_partition() == 0;
    return s_ready ? 0 : -1;
}

static uint16_t fat12_next(uint16_t cluster)
{
    uint8_t entry[2];
    uint32_t offset = FAT_START_LBA * DRIVE_BLOCK_SIZE +
                      cluster + cluster / 2u;
    uint16_t value;

    if (drive_read(offset, entry, sizeof(entry)) != 0) return 0xFFF;
    value = get16(entry, 0);
    return (cluster & 1u) ? (uint16_t)(value >> 4)
                          : (uint16_t)(value & 0x0FFFu);
}

static int load_file_83(uint8_t const target[11], char *buffer,
                        size_t capacity, size_t *length_out)
{
    uint8_t entry[32];
    uint32_t index;
    uint32_t size = 0;
    uint16_t cluster = 0;
    size_t copied = 0;
    uint32_t guard = 0;

    if (length_out) *length_out = 0;
    if (!s_ready || !buffer || capacity < 2 || cache_flush() != 0)
        return YAJIR_AUTORUN_ERROR;

    for (index = 0; index < 64; ++index) {
        uint32_t offset = ROOT_START_LBA * DRIVE_BLOCK_SIZE + index * 32u;

        if (drive_read(offset, entry, sizeof(entry)) != 0)
            return YAJIR_AUTORUN_ERROR;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5 || entry[11] == 0x0F || (entry[11] & 0x18))
            continue;
        if (memcmp(entry, target, 11u) == 0) {
            cluster = get16(entry, 26);
            size = get32(entry, 28);
            break;
        }
    }

    if (size == 0 && cluster == 0) return YAJIR_AUTORUN_NOT_FOUND;
    if (size >= capacity) return YAJIR_AUTORUN_TOO_LARGE;
    if (size > 0 && cluster < 2) return YAJIR_AUTORUN_ERROR;

    while (copied < size) {
        uint32_t lba;
        size_t chunk = size - copied;

        if (cluster < 2 || cluster >= 0xFF8 || ++guard > DRIVE_BLOCK_COUNT)
            return YAJIR_AUTORUN_ERROR;
        if (chunk > DRIVE_BLOCK_SIZE) chunk = DRIVE_BLOCK_SIZE;
        lba = DATA_START_LBA + (uint32_t)(cluster - 2u);
        if (lba >= DRIVE_BLOCK_COUNT ||
            drive_read(lba * DRIVE_BLOCK_SIZE, buffer + copied, chunk) != 0)
            return YAJIR_AUTORUN_ERROR;
        copied += chunk;
        if (copied < size) cluster = fat12_next(cluster);
    }

    buffer[copied] = '\0';
    if (length_out) *length_out = copied;
    return YAJIR_AUTORUN_FOUND;
}

int yajir_drive_load_autorun(char *buffer, size_t capacity,
                             size_t *length_out)
{
    static uint8_t const target[11] = {
        'A','U','T','O','R','U','N',' ','Y','A','J'
    };

    return load_file_83(target, buffer, capacity, length_out);
}

static int library_target(const char *name, uint8_t target[11])
{
    size_t length = 0;

    if (!name || !name[0]) return -1;
    memset(target, ' ', 8);
    while (*name) {
        uint8_t c = (uint8_t)*name++;

        if (length >= 8u) return -1;
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return -1;
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        target[length++] = c;
    }
    target[8] = 'Y';
    target[9] = 'A';
    target[10] = 'J';
    return 0;
}

int yajir_drive_import(const char *name, const char **source,
                       uint32_t *length)
{
    uint8_t target[11];
    size_t source_length = 0;

    if (!source || !length || library_target(name, target) != 0)
        return -1;

    /* Import解析中はMSC書込みキャッシュをソースバッファとして借りる。
     * 先にflushしてキャッシュとの対応を外せば、load_file_83の読込み元はXIPだけになり、
     * 同じ4 KiBを安全に上書きできる。コンパイラは復帰後に原文を保持しない。 */
    if (cache_flush() != 0) return -1;
    s_cache_block = DRIVE_CACHE_NONE;
    if (load_file_83(target, (char *)s_cache, sizeof(s_cache),
                     &source_length) != YAJIR_AUTORUN_FOUND)
        return -1;
    *source = (char const *)s_cache;
    *length = (uint32_t)source_length;
    return 0;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "Yajir   ", 8);
    memcpy(product_id, "Script Drive    ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    if (!s_ready || s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size)
{
    (void)lun;
    *block_count = DRIVE_BLOCK_COUNT;
    *block_size = DRIVE_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    if (load_eject) {
        if (!start) cache_flush();
        s_ejected = !start;
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t size)
{
    (void)lun;
    if (lba >= DRIVE_BLOCK_COUNT || offset > DRIVE_BLOCK_SIZE ||
        size > DRIVE_BLOCK_SIZE - offset ||
        size > DRIVE_SIZE - lba * DRIVE_BLOCK_SIZE - offset)
        return -1;
    return drive_read(lba * DRIVE_BLOCK_SIZE + offset, buffer, size) == 0
               ? (int32_t)size : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t size)
{
    (void)lun;
    if (lba >= DRIVE_BLOCK_COUNT || offset > DRIVE_BLOCK_SIZE ||
        size > DRIVE_BLOCK_SIZE - offset ||
        size > DRIVE_SIZE - lba * DRIVE_BLOCK_SIZE - offset)
        return -1;
    return drive_write(lba * DRIVE_BLOCK_SIZE + offset, buffer, size) == 0
               ? (int32_t)size : -1;
}

void tud_msc_write10_complete_cb(uint8_t lun)
{
    (void)lun;
    cache_flush();
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const command[16],
                        void *buffer, uint16_t size)
{
    (void)buffer;
    (void)size;
#ifdef SCSI_CMD_SYNCHRONIZE_CACHE_10
    if (command[0] == SCSI_CMD_SYNCHRONIZE_CACHE_10)
        return cache_flush() == 0 ? 0 : -1;
#endif
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}
