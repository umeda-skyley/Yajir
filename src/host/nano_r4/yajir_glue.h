/* yajir_glue.h - Arduino Nano R4 board bindings */
#ifndef YAJIR_GLUE_NANO_R4_H
#define YAJIR_GLUE_NANO_R4_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void yajir_glue_init(void);
void yajir_glue_stop(void);
void host_register_all(void);

int32_t get_tick(void);
void yajir_putc(char c);
void yajir_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* YAJIR_GLUE_NANO_R4_H */
