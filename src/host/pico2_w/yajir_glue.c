/* yajir_glue.c - Pico 2 W board and peripheral ports */
#include <stddef.h>

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"

#include "host_diag.h"
#include "script.h"
#include "vm.h"
#include "yajir_glue.h"

static bool s_cyw43_ready;
static int32_t s_led1;

enum {
    YAJIR_GPIO_IN = 0,
    YAJIR_GPIO_OUT = 1,
    YAJIR_GPIO_IN_PULLUP = 2,
    YAJIR_GPIO_IN_PULLDOWN = 3
};

enum {
    YAJIR_GPIO_IRQ_RISE = 1,
    YAJIR_GPIO_IRQ_FALL = 2,
    YAJIR_GPIO_IRQ_HIGH = 4,
    YAJIR_GPIO_IRQ_LOW = 8
};

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

static void reg_const(const char *name, int32_t value)
{
    script_register_const(name, value);
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

static bool gpio_is_exposed(int32_t pin)
{
    return (pin >= 0 && pin <= 22) || (pin >= 26 && pin <= 28);
}

static void th_gpio_get(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;

    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    script_set_result(gpio_get((uint)pin) ? 1 : 0);
}

static void th_gpio_set(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t value = argc > 1 && argv[1].i != 0;

    if (argc < 2 || !gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    gpio_put((uint)pin, value != 0);
    script_set_result(value);
}

static void th_gpio_mode(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t mode = argc > 1 ? argv[1].i : -1;

    if (argc < 2 || !gpio_is_exposed(pin) ||
        mode < YAJIR_GPIO_IN || mode > YAJIR_GPIO_IN_PULLDOWN) {
        script_set_result(-1);
        return;
    }

    gpio_init((uint)pin);
    gpio_disable_pulls((uint)pin);
    if (mode == YAJIR_GPIO_OUT) {
        gpio_set_dir((uint)pin, GPIO_OUT);
    } else {
        gpio_set_dir((uint)pin, GPIO_IN);
        if (mode == YAJIR_GPIO_IN_PULLUP)
            gpio_pull_up((uint)pin);
        else if (mode == YAJIR_GPIO_IN_PULLDOWN)
            gpio_pull_down((uint)pin);
    }
    script_set_result(0);
}

static void th_gpio_toggle(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t value;

    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    value = gpio_get_out_level((uint)pin) ? 0 : 1;
    gpio_put((uint)pin, value != 0);
    script_set_result(value);
}

static uint32_t gpio_irq_to_sdk(int32_t mask)
{
    uint32_t events = 0;

    if (mask & YAJIR_GPIO_IRQ_RISE) events |= GPIO_IRQ_EDGE_RISE;
    if (mask & YAJIR_GPIO_IRQ_FALL) events |= GPIO_IRQ_EDGE_FALL;
    if (mask & YAJIR_GPIO_IRQ_HIGH) events |= GPIO_IRQ_LEVEL_HIGH;
    if (mask & YAJIR_GPIO_IRQ_LOW) events |= GPIO_IRQ_LEVEL_LOW;
    return events;
}

static int32_t gpio_irq_from_sdk(uint32_t events)
{
    int32_t mask = 0;

    if (events & GPIO_IRQ_EDGE_RISE) mask |= YAJIR_GPIO_IRQ_RISE;
    if (events & GPIO_IRQ_EDGE_FALL) mask |= YAJIR_GPIO_IRQ_FALL;
    if (events & GPIO_IRQ_LEVEL_HIGH) mask |= YAJIR_GPIO_IRQ_HIGH;
    if (events & GPIO_IRQ_LEVEL_LOW) mask |= YAJIR_GPIO_IRQ_LOW;
    return mask;
}

static void gpio_irq_callback(uint gpio, uint32_t events)
{
    script_arg_t args[3];

    args[0] = SCRIPT_ARG_INT((int32_t)gpio);
    args[1] = SCRIPT_ARG_INT(gpio_irq_from_sdk(events));
    args[2] = SCRIPT_ARG_INT(gpio_get(gpio) ? 1 : 0);
    script_post_msg_v("GPIO_IRQ", 3, args);
}

static void th_gpio_irq_enable(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t mask = argc > 1 ? argv[1].i : 0;
    uint32_t events = gpio_irq_to_sdk(mask);

    if (argc < 2 || !gpio_is_exposed(pin) || events == 0 ||
        (mask & ~15) != 0) {
        script_set_result(-1);
        return;
    }
    gpio_set_irq_enabled_with_callback((uint)pin, events, true,
                                       gpio_irq_callback);
    script_set_result(0);
}

static void th_gpio_irq_disable(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    uint32_t all_events = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL |
                          GPIO_IRQ_LEVEL_HIGH | GPIO_IRQ_LEVEL_LOW;

    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    gpio_set_irq_enabled((uint)pin, all_events, false);
    script_set_result(0);
}

static void th_adc_get(int argc, const script_value_t *argv)
{
    int32_t channel = argc > 0 ? argv[0].i : -1;

    if (channel < 0 || channel > 2) {
        script_set_result(-1);
        return;
    }
    adc_gpio_init((uint)(26 + channel));
    adc_select_input((uint)channel);
    script_set_result((int32_t)adc_read());
}

static void th_adc_pin(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;

    if (pin < 26 || pin > 28) {
        script_set_result(-1);
        return;
    }
    adc_gpio_init((uint)pin);
    adc_select_input((uint)(pin - 26));
    script_set_result((int32_t)adc_read());
}

static int32_t adc_temp_get(void)
{
    int32_t raw;
    int64_t voltage_uv;
    int64_t delta_uv;

    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    raw = (int32_t)adc_read();
    voltage_uv = ((int64_t)raw * 3300000 + 2047) / 4095;
    delta_uv = voltage_uv - 706000;
    return (int32_t)(2700 - (delta_uv * 100) / 1721);
}

void host_register_all(void)
{
    host_diag_reset();

    reg_inout("LED1", led1_get, led1_set, SCRIPT_T_INT);
    reg_in("NOW", get_tick, SCRIPT_T_INT);
    reg_in("VMSIZE", get_vmsize, SCRIPT_T_INT);
    reg_inout("GPIO_GET", NULL, th_gpio_get, SCRIPT_T_INT);
    reg_inout("GPIO_SET", NULL, th_gpio_set, SCRIPT_T_INT);
    reg_out("GPIO_MODE", th_gpio_mode);
    reg_inout("GPIO_TOGGLE", NULL, th_gpio_toggle, SCRIPT_T_INT);
    reg_out("GPIO_IRQ_ENABLE", th_gpio_irq_enable);
    reg_out("GPIO_IRQ_DISABLE", th_gpio_irq_disable);
    reg_inout("ADC_GET", NULL, th_adc_get, SCRIPT_T_INT);
    reg_inout("ADC_PIN", NULL, th_adc_pin, SCRIPT_T_INT);
    reg_in("ADC_TEMP", adc_temp_get, SCRIPT_T_INT);
    reg_out("STDOUT", th_stdout);
    reg_out("DELAY", th_delay);
    reg_handler("USB_SERIAL");
    reg_handler("GPIO_IRQ");

    reg_const("GPIO_IN", YAJIR_GPIO_IN);
    reg_const("GPIO_OUT", YAJIR_GPIO_OUT);
    reg_const("GPIO_IN_PULLUP", YAJIR_GPIO_IN_PULLUP);
    reg_const("GPIO_IN_PULLDOWN", YAJIR_GPIO_IN_PULLDOWN);
    reg_const("GPIO_IRQ_RISE", YAJIR_GPIO_IRQ_RISE);
    reg_const("GPIO_IRQ_FALL", YAJIR_GPIO_IRQ_FALL);
    reg_const("GPIO_IRQ_HIGH", YAJIR_GPIO_IRQ_HIGH);
    reg_const("GPIO_IRQ_LOW", YAJIR_GPIO_IRQ_LOW);
}

void yajir_glue_init(bool cyw43_ready)
{
    s_cyw43_ready = cyw43_ready;
    s_led1 = 0;
    adc_init();
    adc_set_temp_sensor_enabled(true);
    if (s_cyw43_ready)
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
}

void yajir_glue_stop(void)
{
    uint32_t all_events = GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL |
                          GPIO_IRQ_LEVEL_HIGH | GPIO_IRQ_LEVEL_LOW;
    uint pin;

    for (pin = 0; pin <= 28; ++pin) {
        if (gpio_is_exposed((int32_t)pin))
            gpio_set_irq_enabled(pin, all_events, false);
    }
}
