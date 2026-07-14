/* yajir_loader.h - USB serial script loader for Pico 2 W */
#ifndef YAJIR_LOADER_PICO2_W_H
#define YAJIR_LOADER_PICO2_W_H

#include <stdint.h>

void yajir_loader_init(void);
void yajir_loader_feed_byte(uint8_t byte);
void yajir_loader_tick(void);

#endif /* YAJIR_LOADER_PICO2_W_H */
