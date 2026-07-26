/* yajir_loader.h - USB serial script loader for M5Stack Cardputer */
#ifndef YAJIR_LOADER_CARDPUTER_H
#define YAJIR_LOADER_CARDPUTER_H

#include <stdint.h>

void yajir_loader_init(void);
void yajir_loader_feed_byte(uint8_t byte);
void yajir_loader_feed_key(uint8_t byte);
void yajir_loader_tick(void);

#endif /* YAJIR_LOADER_CARDPUTER_H */
