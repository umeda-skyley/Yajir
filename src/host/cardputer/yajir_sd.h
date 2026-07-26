/* yajir_sd.h - Cardputer microSD autorun and import support */
#ifndef YAJIR_SD_CARDPUTER_H
#define YAJIR_SD_CARDPUTER_H

#include <stddef.h>
#include <stdint.h>

enum yajir_sd_load_status_t {
    YAJIR_SD_ERROR = -1,
    YAJIR_SD_TOO_LARGE = -2,
    YAJIR_SD_INVALID_NAME = -3,
    YAJIR_SD_NOT_FOUND = 0,
    YAJIR_SD_FOUND = 1
};

int yajir_sd_init(void);
int32_t yajir_sd_ready(void);
int32_t yajir_sd_size_mb(void);
int yajir_sd_load_autorun(char *buffer, size_t capacity, size_t *length);
int yajir_sd_load_script(const char *name, char *buffer, size_t capacity,
                         size_t *length);
int yajir_sd_import(const char *name, const char **source, uint32_t *length);

#endif /* YAJIR_SD_CARDPUTER_H */
