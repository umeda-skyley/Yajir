/* yajir_glue.h - Pico 2 W board bindings */
#ifndef YAJIR_GLUE_PICO2_W_H
#define YAJIR_GLUE_PICO2_W_H

#include <stdbool.h>
#include <stdint.h>

void yajir_glue_init(bool cyw43_ready);
void yajir_glue_stop(void);
void host_register_all(void);

int32_t get_tick(void);
void yajir_putc(char c);
void yajir_puts(const char *s);

#endif /* YAJIR_GLUE_PICO2_W_H */
