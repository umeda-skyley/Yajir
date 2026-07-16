/* main.c - Pico 2 W USB serial host entry point */
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "yajir_glue.h"
#include "yajir_drive.h"
#include "yajir_loader.h"
#include "yajir_usb.h"

int main(void)
{
    bool cyw43_ready;

    cyw43_ready = cyw43_arch_init() == 0;
    yajir_glue_init(cyw43_ready);
    yajir_drive_init();
    yajir_usb_init();

    for (int i = 0; i < 1500; ++i) {
        yajir_usb_task();
        sleep_ms(1);
    }

    yajir_loader_init();

    if (!cyw43_ready)
        yajir_puts("[board] CYW43 initialization failed; LED1 is unavailable.\r\n");

    for (;;) {
        uint8_t byte;

        yajir_usb_task();
        while (yajir_usb_getc(&byte))
            yajir_loader_feed_byte(byte);

        yajir_loader_tick();
        sleep_ms(1);
    }
}
