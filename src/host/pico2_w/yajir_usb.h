#ifndef YAJIR_USB_H
#define YAJIR_USB_H

#include <stdbool.h>
#include <stdint.h>

void yajir_usb_init(void);
void yajir_usb_task(void);
void yajir_usb_putc(char c);
bool yajir_usb_getc(uint8_t *value);

#endif /* YAJIR_USB_H */
