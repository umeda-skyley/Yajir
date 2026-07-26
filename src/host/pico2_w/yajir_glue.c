/* yajir_glue.c - Pico 2 W board and peripheral ports */
#include <stddef.h>

#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/timer.h"
#include "pico/low_power.h"
#include "pico/time.h"
#include "pico/cyw43_arch.h"

#include "host_diag.h"
#include "script.h"
#include "yajir_drive.h"
#include "yajir_glue.h"
#include "yajir_usb.h"

static bool s_cyw43_ready;
static int32_t s_led1;

#define YAJIR_PWM_DEFAULT_HZ 1000
#define YAJIR_PWM_LEVEL_MAX 65535
#define YAJIR_I2C_TIMEOUT_US 20000
#define YAJIR_PULSE_MAX_US 10000000

typedef struct {
    uint16_t level[2];
    uint16_t wrap;
    uint32_t frequency;
    bool initialized;
} yajir_pwm_slice_t;

static yajir_pwm_slice_t s_pwm[NUM_PWM_SLICES];
static uint32_t s_pwm_pins;

typedef struct {
    int8_t sda_pin;
    int8_t scl_pin;
    bool initialized;
} yajir_i2c_t;

static yajir_i2c_t s_i2c0;

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
    yajir_usb_putc(c);
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

static void th_delay(int argc, const script_value_t *argv)
{
    int32_t ms = argc > 0 ? argv[0].i : 0;
    if (ms > 0) sleep_ms((uint32_t)ms);
}

static int64_t sleep_alarm_callback(alarm_id_t id, void *user_data)
{
    (void)id;
    (void)user_data;
    return 0;
}

static void th_sleep(int argc, const script_value_t *argv)
{
    int32_t ms = argc > 0 ? argv[0].i : 0;
    alarm_id_t alarm_id = -1;
    clock_dest_bitset_t keep_enabled = clock_dest_bitset_none();
    timer_hw_t *timer = PICO_DEFAULT_TIMER_INSTANCE();

    clock_dest_bitset_add(&keep_enabled,
                          timer_get_index(timer) ? CLK_DEST_SYS_TIMER1
                                                 : CLK_DEST_SYS_TIMER0);
    clock_dest_bitset_add(&keep_enabled, CLK_DEST_REF_TICKS);

    if (ms > 0) {
        alarm_id = add_alarm_in_ms((uint32_t)ms, sleep_alarm_callback,
                                   NULL, true);
        if (alarm_id < 0) return;
    }

    low_power_sleep_until_irq(&keep_enabled);

    if (alarm_id >= 0) cancel_alarm(alarm_id);
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

static void th_gpio_pulse(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t level = argc > 1 ? argv[1].i : -1;
    int32_t width_us = argc > 2 ? argv[2].i : -1;

    if (argc < 3 || !gpio_is_exposed(pin) ||
        (level != 0 && level != 1) ||
        width_us < 1 || width_us > YAJIR_PULSE_MAX_US) {
        script_set_result(-1);
        return;
    }

    gpio_set_function((uint)pin, GPIO_FUNC_SIO);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, level == 0);
    busy_wait_us_32(2);
    gpio_put((uint)pin, level != 0);
    busy_wait_us_32((uint32_t)width_us);
    gpio_put((uint)pin, level == 0);
    script_set_result(width_us);
}

static void th_pulse_in(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t level = argc > 1 ? argv[1].i : -1;
    int32_t timeout_us = argc > 2 ? argv[2].i : -1;
    uint64_t deadline;
    uint64_t started;

    if (argc < 3 || !gpio_is_exposed(pin) ||
        (level != 0 && level != 1) ||
        timeout_us < 1 || timeout_us > YAJIR_PULSE_MAX_US) {
        script_set_result(-1);
        return;
    }

    gpio_set_function((uint)pin, GPIO_FUNC_SIO);
    gpio_set_dir((uint)pin, GPIO_IN);
    deadline = time_us_64() + (uint32_t)timeout_us;

    while ((gpio_get((uint)pin) ? 1 : 0) == level) {
        if (time_us_64() >= deadline) {
            script_set_result(0);
            return;
        }
        tight_loop_contents();
    }
    while ((gpio_get((uint)pin) ? 1 : 0) != level) {
        if (time_us_64() >= deadline) {
            script_set_result(0);
            return;
        }
        tight_loop_contents();
    }

    started = time_us_64();
    while ((gpio_get((uint)pin) ? 1 : 0) == level) {
        if (time_us_64() >= deadline) {
            script_set_result(0);
            return;
        }
        tight_loop_contents();
    }
    script_set_result((int32_t)(time_us_64() - started));
}

static bool i2c0_pins_are_valid(int32_t sda_pin, int32_t scl_pin)
{
    return gpio_is_exposed(sda_pin) && gpio_is_exposed(scl_pin) &&
           (sda_pin & 3) == 0 && scl_pin == sda_pin + 1;
}

static void i2c0_reset(void)
{
    if (!s_i2c0.initialized) return;

    i2c_deinit(i2c0);
    gpio_set_function((uint)s_i2c0.sda_pin, GPIO_FUNC_SIO);
    gpio_set_function((uint)s_i2c0.scl_pin, GPIO_FUNC_SIO);
    gpio_disable_pulls((uint)s_i2c0.sda_pin);
    gpio_disable_pulls((uint)s_i2c0.scl_pin);
    gpio_set_dir((uint)s_i2c0.sda_pin, GPIO_IN);
    gpio_set_dir((uint)s_i2c0.scl_pin, GPIO_IN);
    s_i2c0.sda_pin = -1;
    s_i2c0.scl_pin = -1;
    s_i2c0.initialized = false;
}

static void th_i2c0_open(int argc, const script_value_t *argv)
{
    int32_t sda_pin = argc > 0 ? argv[0].i : -1;
    int32_t scl_pin = argc > 1 ? argv[1].i : -1;
    int32_t frequency = argc > 2 ? argv[2].i : -1;
    uint actual;

    if (argc < 3 || !i2c0_pins_are_valid(sda_pin, scl_pin) ||
        frequency < 1000 || frequency > 1000000) {
        script_set_result(-1);
        return;
    }

    i2c0_reset();
    actual = i2c_init(i2c0, (uint)frequency);
    gpio_set_function((uint)sda_pin, GPIO_FUNC_I2C);
    gpio_set_function((uint)scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up((uint)sda_pin);
    gpio_pull_up((uint)scl_pin);
    s_i2c0.sda_pin = (int8_t)sda_pin;
    s_i2c0.scl_pin = (int8_t)scl_pin;
    s_i2c0.initialized = true;
    script_set_result((int32_t)actual);
}

static bool i2c_address_is_valid(int32_t address)
{
    return address >= 0x08 && address <= 0x77;
}

static void th_i2c0_write(int argc, const script_value_t *argv)
{
    uint8_t data[CFG_ARG_COUNT - 1];
    int32_t address = argc > 0 ? argv[0].i : -1;
    int count;
    int i;

    if (!s_i2c0.initialized || argc < 2 ||
        !i2c_address_is_valid(address)) {
        script_set_result(-1);
        return;
    }
    count = argc - 1;
    for (i = 0; i < count; ++i) {
        if (argv[i + 1].i < 0 || argv[i + 1].i > 255) {
            script_set_result(-1);
            return;
        }
        data[i] = (uint8_t)argv[i + 1].i;
    }
    script_set_result(i2c_write_timeout_us(i2c0, (uint8_t)address,
                                           data, (size_t)count, false,
                                           YAJIR_I2C_TIMEOUT_US));
}

static void th_i2c0_write8(int argc, const script_value_t *argv)
{
    uint8_t data[2];
    int32_t address = argc > 0 ? argv[0].i : -1;

    if (!s_i2c0.initialized || argc < 3 ||
        !i2c_address_is_valid(address) ||
        argv[1].i < 0 || argv[1].i > 255 ||
        argv[2].i < 0 || argv[2].i > 255) {
        script_set_result(-1);
        return;
    }
    data[0] = (uint8_t)argv[1].i;
    data[1] = (uint8_t)argv[2].i;
    script_set_result(i2c_write_timeout_us(i2c0, (uint8_t)address,
                                           data, 2, false,
                                           YAJIR_I2C_TIMEOUT_US));
}

static void th_i2c0_read8(int argc, const script_value_t *argv)
{
    uint8_t reg;
    uint8_t value;
    int32_t address = argc > 0 ? argv[0].i : -1;
    int result;

    if (!s_i2c0.initialized || argc < 2 ||
        !i2c_address_is_valid(address) ||
        argv[1].i < 0 || argv[1].i > 255) {
        script_set_result(-1);
        return;
    }
    reg = (uint8_t)argv[1].i;
    result = i2c_write_timeout_us(i2c0, (uint8_t)address, &reg, 1, true,
                                  YAJIR_I2C_TIMEOUT_US);
    if (result != 1) {
        script_set_result(-1);
        return;
    }
    result = i2c_read_timeout_us(i2c0, (uint8_t)address, &value, 1, false,
                                 YAJIR_I2C_TIMEOUT_US);
    script_set_result(result == 1 ? (int32_t)value : -1);
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

static uint16_t pwm_compare_level(uint16_t level, uint16_t wrap)
{
    uint32_t period = (uint32_t)wrap + 1u;
    uint32_t compare = ((uint32_t)level * period + 32767u) / 65535u;

    return (uint16_t)(compare > 65535u ? 65535u : compare);
}

static void pwm_restore_levels(uint slice)
{
    pwm_set_chan_level(slice, PWM_CHAN_A,
                       pwm_compare_level(s_pwm[slice].level[PWM_CHAN_A],
                                         s_pwm[slice].wrap));
    pwm_set_chan_level(slice, PWM_CHAN_B,
                       pwm_compare_level(s_pwm[slice].level[PWM_CHAN_B],
                                         s_pwm[slice].wrap));
}

static int32_t pwm_configure_frequency(uint slice, int32_t requested_hz)
{
    uint32_t sys_hz = clock_get_hz(clk_sys);
    uint64_t scaled_clock = (uint64_t)sys_hz * 16u;
    uint64_t target_ticks;
    uint32_t divider16;
    uint32_t period;
    uint64_t divisor;
    uint32_t actual_hz;

    if (requested_hz <= 0 || (uint32_t)requested_hz > sys_hz)
        return -1;

    target_ticks = (scaled_clock + (uint32_t)requested_hz / 2u) /
                   (uint32_t)requested_hz;
    if (target_ticks > (uint64_t)4095u * 65536u)
        return -1;

    divider16 = (uint32_t)((target_ticks + 65535u) / 65536u);
    if (divider16 < 16u) divider16 = 16u;
    if (divider16 > 4095u) return -1;

    period = (uint32_t)((target_ticks + divider16 / 2u) / divider16);
    if (period < 1u) period = 1u;
    if (period > 65536u) period = 65536u;

    divisor = (uint64_t)divider16 * period;
    actual_hz = (uint32_t)((scaled_clock + divisor / 2u) / divisor);

    pwm_set_enabled(slice, false);
    pwm_set_clkdiv_int_frac4(slice, (uint8_t)(divider16 / 16u),
                            (uint8_t)(divider16 % 16u));
    pwm_set_wrap(slice, (uint16_t)(period - 1u));
    s_pwm[slice].wrap = (uint16_t)(period - 1u);
    s_pwm[slice].frequency = actual_hz;
    pwm_restore_levels(slice);
    pwm_set_enabled(slice, true);
    return (int32_t)actual_hz;
}

static int pwm_prepare_slice(uint slice)
{
    if (s_pwm[slice].initialized) return 0;

    s_pwm[slice].level[PWM_CHAN_A] = 0;
    s_pwm[slice].level[PWM_CHAN_B] = 0;
    if (pwm_configure_frequency(slice, YAJIR_PWM_DEFAULT_HZ) < 0)
        return -1;
    s_pwm[slice].initialized = true;
    return 0;
}

static void pwm_prepare_pin(uint pin)
{
    gpio_set_function(pin, GPIO_FUNC_PWM);
    s_pwm_pins |= 1u << pin;
}

static void th_pwm_set(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t level = argc > 1 ? argv[1].i : -1;
    uint slice;
    uint channel;

    if (argc < 2 || !gpio_is_exposed(pin) ||
        level < 0 || level > YAJIR_PWM_LEVEL_MAX) {
        script_set_result(-1);
        return;
    }

    slice = pwm_gpio_to_slice_num((uint)pin);
    channel = pwm_gpio_to_channel((uint)pin);
    if (pwm_prepare_slice(slice) != 0) {
        script_set_result(-1);
        return;
    }

    pwm_prepare_pin((uint)pin);
    s_pwm[slice].level[channel] = (uint16_t)level;
    pwm_set_chan_level(slice, channel,
                       pwm_compare_level((uint16_t)level,
                                         s_pwm[slice].wrap));
    script_set_result(level);
}

static void th_pwm_get(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    uint slice;
    uint channel;

    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }

    slice = pwm_gpio_to_slice_num((uint)pin);
    channel = pwm_gpio_to_channel((uint)pin);
    script_set_result((int32_t)s_pwm[slice].level[channel]);
}

static void th_pwm_freq(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t frequency = argc > 1 ? argv[1].i : -1;
    uint slice;
    int32_t actual;

    if (argc < 2 || !gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }

    slice = pwm_gpio_to_slice_num((uint)pin);
    actual = pwm_configure_frequency(slice, frequency);
    if (actual < 0) {
        script_set_result(-1);
        return;
    }

    s_pwm[slice].initialized = true;
    pwm_prepare_pin((uint)pin);
    script_set_result(actual);
}

static void pwm_reset(void)
{
    uint slice;

    for (slice = 0; slice < NUM_PWM_SLICES; ++slice) {
        pwm_set_enabled(slice, false);
        s_pwm[slice].level[PWM_CHAN_A] = 0;
        s_pwm[slice].level[PWM_CHAN_B] = 0;
        s_pwm[slice].wrap = 0;
        s_pwm[slice].frequency = 0;
        s_pwm[slice].initialized = false;
    }
    s_pwm_pins = 0;
}

void host_register_all(void)
{
    host_diag_reset();

    reg_inout("LED1", led1_get, led1_set, SCRIPT_T_INT);
    script_register_now(get_tick);
    reg_inout("GPIO_GET", NULL, th_gpio_get, SCRIPT_T_INT);
    reg_inout("GPIO_SET", NULL, th_gpio_set, SCRIPT_T_INT);
    reg_out("GPIO_MODE", th_gpio_mode);
    reg_inout("GPIO_TOGGLE", NULL, th_gpio_toggle, SCRIPT_T_INT);
    reg_inout("GPIO_PULSE", NULL, th_gpio_pulse, SCRIPT_T_INT);
    reg_inout("PULSE_IN", NULL, th_pulse_in, SCRIPT_T_INT);
    reg_out("GPIO_IRQ_ENABLE", th_gpio_irq_enable);
    reg_out("GPIO_IRQ_DISABLE", th_gpio_irq_disable);
    reg_inout("ADC_GET", NULL, th_adc_get, SCRIPT_T_INT);
    reg_inout("ADC_PIN", NULL, th_adc_pin, SCRIPT_T_INT);
    reg_in("ADC_TEMP", adc_temp_get, SCRIPT_T_INT);
    reg_inout("PWM_SET", NULL, th_pwm_set, SCRIPT_T_INT);
    reg_inout("PWM_GET", NULL, th_pwm_get, SCRIPT_T_INT);
    reg_out("PWM_FREQ", th_pwm_freq);
    reg_inout("I2C0_OPEN", NULL, th_i2c0_open, SCRIPT_T_INT);
    reg_inout("I2C0_WRITE", NULL, th_i2c0_write, SCRIPT_T_INT);
    reg_inout("I2C0_WRITE8", NULL, th_i2c0_write8, SCRIPT_T_INT);
    reg_inout("I2C0_READ8", NULL, th_i2c0_read8, SCRIPT_T_INT);
    script_register_stdout(yajir_puts);
    script_register_import(yajir_drive_import);
    reg_out("DELAY", th_delay);
    reg_out("SLEEP", th_sleep);
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
    reg_const("ADC_MAX", 4095);
    reg_const("PWM_MAX", YAJIR_PWM_LEVEL_MAX);
    reg_const("PWM_DEFAULT_FREQ", YAJIR_PWM_DEFAULT_HZ);
}

void yajir_glue_init(bool cyw43_ready)
{
    s_cyw43_ready = cyw43_ready;
    s_led1 = 0;
    s_i2c0.sda_pin = -1;
    s_i2c0.scl_pin = -1;
    s_i2c0.initialized = false;
    pwm_reset();
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

    for (pin = 0; pin <= 28; ++pin) {
        if ((s_pwm_pins & (1u << pin)) == 0) continue;
        gpio_set_function(pin, GPIO_FUNC_SIO);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, false);
    }
    i2c0_reset();
    pwm_reset();
}
