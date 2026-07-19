/* yajir_loader.h - USB serial script loader for Arduino Nano R4 */
#ifndef YAJIR_LOADER_NANO_R4_H
#define YAJIR_LOADER_NANO_R4_H

#include <stdint.h>

void yajir_loader_init(void);
void yajir_loader_feed_byte(uint8_t byte);
void yajir_loader_tick(void);

#endif /* YAJIR_LOADER_NANO_R4_H */
