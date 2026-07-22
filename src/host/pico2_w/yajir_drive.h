#ifndef YAJIR_DRIVE_H
#define YAJIR_DRIVE_H

#include <stddef.h>
#include <stdint.h>

enum {
    YAJIR_AUTORUN_ERROR = -1,
    YAJIR_AUTORUN_TOO_LARGE = -2,
    YAJIR_AUTORUN_NOT_FOUND = 0,
    YAJIR_AUTORUN_FOUND = 1
};

int yajir_drive_init(void);
int yajir_drive_load_autorun(char *buffer, size_t capacity,
                             size_t *length_out);
int yajir_drive_import(const char *name, const char **source,
                       uint32_t *length);

#endif /* YAJIR_DRIVE_H */
