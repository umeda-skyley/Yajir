/* main.c - Pico 2 W USB serial host entry point */
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "yajir_glue.h"
#include "yajir_loader.h"

int main(void)
{
    bool cyw43_ready;

    stdio_init_all();
    sleep_ms(1500);

    cyw43_ready = cyw43_arch_init() == 0;
    yajir_glue_init(cyw43_ready);
    yajir_loader_init();

    if (!cyw43_ready)
        yajir_puts("[board] CYW43 initialization failed; LED1 is unavailable.\r\n");

    for (;;) {
        int ch;
        while ((ch = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT)
            yajir_loader_feed_byte((uint8_t)ch);

        yajir_loader_tick();
        sleep_ms(1);
    }
}
