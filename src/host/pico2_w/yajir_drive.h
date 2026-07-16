#ifndef YAJIR_DRIVE_H
#define YAJIR_DRIVE_H

#include <stddef.h>

enum {
    YAJIR_AUTORUN_ERROR = -1,
    YAJIR_AUTORUN_TOO_LARGE = -2,
    YAJIR_AUTORUN_NOT_FOUND = 0,
    YAJIR_AUTORUN_FOUND = 1
};

int yajir_drive_init(void);
int yajir_drive_load_autorun(char *buffer, size_t capacity,
                             size_t *length_out);

#endif /* YAJIR_DRIVE_H */
