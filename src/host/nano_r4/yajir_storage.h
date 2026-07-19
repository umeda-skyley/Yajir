/* yajir_storage.h - Nano R4 data-flash autorun storage */
#ifndef YAJIR_STORAGE_NANO_R4_H
#define YAJIR_STORAGE_NANO_R4_H

#include <stddef.h>

enum yajir_storage_status_t {
    YAJIR_STORAGE_ERROR = -1,
    YAJIR_STORAGE_EMPTY = 0,
    YAJIR_STORAGE_OK = 1,
    YAJIR_STORAGE_CORRUPT = 2
};

int yajir_storage_load(char *destination, size_t capacity, size_t *length);
int yajir_storage_save(const char *source, size_t length);
int yajir_storage_erase(void);

#endif /* YAJIR_STORAGE_NANO_R4_H */
