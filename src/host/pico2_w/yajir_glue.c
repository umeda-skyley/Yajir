/* yajir_glue.c - Pico 2 W ports used by the first bring-up stage */
#include <stddef.h>

#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"

#include "host_diag.h"
#include "script.h"
#include "vm.h"
#include "yajir_glue.h"

static bool s_cyw43_ready;
static int32_t s_led1;

void yajir_putc(char c)
{
    putchar_raw((unsigned char)c);
}

void yajir_puts(const char *s)
{
    if (!s) return;
    while (*s) yajir_putc(*s++);
}

int32_t get_tick(void)
{
    return (int32_t)to_ms_since_boot(get_absolute_time());
}

int32_t get_vmsize(void)
{
    return (int32_t)sizeof(script_vm_t);
}

static void reg_out(const char *name, script_out_fn fn)
{
    script_register_out(name, fn);
    host_diag_note(name);
}

static void reg_in(const char *name, script_in_fn fn, script_type_t type)
{
    script_register_in(name, fn, type);
    host_diag_note(name);
}

static void reg_inout(const char *name, script_in_fn get_fn,
                      script_out_fn set_fn, script_type_t type)
{
    script_register_inout(name, get_fn, set_fn, type);
    host_diag_note(name);
}

static void reg_handler(const char *name)
{
    script_register_handler(name);
    host_diag_note(name);
}

static void int_to_str(int32_t value, char *buf)
{
    char tmp[12];
    uint32_t magnitude;
    int i = 0;
    int negative = value < 0;

    if (negative)
        magnitude = (uint32_t)(-(value + 1)) + 1u;
    else
        magnitude = (uint32_t)value;

    do {
        tmp[i++] = (char)('0' + magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0u);
    if (negative) tmp[i++] = '-';

    {
        int j = 0;
        while (i > 0) buf[j++] = tmp[--i];
        buf[j] = '\0';
    }
}

static void th_stdout(int argc, const script_value_t *argv)
{
    char number[16];
    int i;

    for (i = 0; i < argc; ++i) {
        if (script_val_is_str(argv[i])) {
            yajir_puts(script_resolve_str(argv[i]));
        } else {
            int_to_str(argv[i].i, number);
            yajir_puts(number);
        }
    }
    yajir_puts("\r\n");
}

static void th_delay(int argc, const script_value_t *argv)
{
    int32_t ms = argc > 0 ? argv[0].i : 0;
    if (ms > 0) sleep_ms((uint32_t)ms);
}

static int32_t led1_get(void)
{
    return s_led1;
}

static void led1_set(int argc, const script_value_t *argv)
{
    int32_t value = argc > 0 && argv[0].i != 0;

    if (s_cyw43_ready) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, value != 0);
        s_led1 = value;
    }
    script_set_result(s_led1);
}

void host_register_all(void)
{
    host_diag_reset();

    reg_inout("LED1", led1_get, led1_set, SCRIPT_T_INT);
    reg_in("NOW", get_tick, SCRIPT_T_INT);
    reg_in("VMSIZE", get_vmsize, SCRIPT_T_INT);
    reg_out("STDOUT", th_stdout);
    reg_out("DELAY", th_delay);
    reg_handler("USB_SERIAL");
}

void yajir_glue_init(bool cyw43_ready)
{
    s_cyw43_ready = cyw43_ready;
    s_led1 = 0;
    if (s_cyw43_ready)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
}
