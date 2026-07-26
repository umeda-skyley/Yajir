/* yajir_glue.h - M5Stack Cardputer board bindings */
#ifndef YAJIR_GLUE_CARDPUTER_H
#define YAJIR_GLUE_CARDPUTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void yajir_glue_init(void);
void yajir_glue_stop(void);
typedef void (*yajir_key_input_fn)(uint8_t byte);
void yajir_glue_poll(int script_running, yajir_key_input_fn loader_input);
void host_register_all(void);

int32_t get_tick(void);
void yajir_putc(char c);
void yajir_puts(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* YAJIR_GLUE_CARDPUTER_H */
