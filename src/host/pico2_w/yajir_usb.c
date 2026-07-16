/* yajir_usb.c - TinyUSB CDC transport used by the Pico loader */
#include <stddef.h>

#include "tusb.h"
#include "yajir_usb.h"

#define TX_QUEUE_SIZE 512u

static uint8_t s_tx[TX_QUEUE_SIZE];
static uint16_t s_tx_read;
static uint16_t s_tx_write;
static uint16_t s_tx_count;

static void drain_tx(void)
{
    while (s_tx_count > 0 && tud_cdc_write_available() > 0) {
        tud_cdc_write_char(s_tx[s_tx_read]);
        s_tx_read = (uint16_t)((s_tx_read + 1u) % TX_QUEUE_SIZE);
        --s_tx_count;
    }
    tud_cdc_write_flush();
}

void yajir_usb_init(void)
{
    tusb_rhport_init_t init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };

    s_tx_read = 0;
    s_tx_write = 0;
    s_tx_count = 0;
    tusb_init(0, &init);
}

void yajir_usb_task(void)
{
    tud_task();
    drain_tx();
}

void yajir_usb_putc(char c)
{
    if (s_tx_count >= TX_QUEUE_SIZE) {
        yajir_usb_task();
        if (s_tx_count >= TX_QUEUE_SIZE) return;
    }

    s_tx[s_tx_write] = (uint8_t)c;
    s_tx_write = (uint16_t)((s_tx_write + 1u) % TX_QUEUE_SIZE);
    ++s_tx_count;
}

bool yajir_usb_getc(uint8_t *value)
{
    if (!value || tud_cdc_available() == 0) return false;
    *value = (uint8_t)tud_cdc_read_char();
    return true;
}
