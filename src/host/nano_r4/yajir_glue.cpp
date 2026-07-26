/* yajir_glue.cpp - Arduino Nano R4 GPIO and host bindings */
#include "yajir_build_config.h"
#include <Arduino.h>
#include <FspTimer.h>
#include <string.h>

extern FspTimer *__get_timer_for_channel(int channel);

extern "C" {
#include "script.h"
#include "host_diag.h"
}

#include "yajir_glue.h"
#include "yajir_libs.h"

enum {
    YAJIR_GPIO_IN = 0,
    YAJIR_GPIO_OUT = 1,
    YAJIR_GPIO_IN_PULLUP = 2,
    YAJIR_GPIO_IN_PULLDOWN = 3,
    YAJIR_GPIO_IRQ_RISE = 1,
    YAJIR_GPIO_IRQ_FALL = 2,
    YAJIR_GPIO_IRQ_LOW = 8,
    YAJIR_ADC_MAX = 16383,
    YAJIR_PWM_MAX = 65535,
    YAJIR_PWM_DEFAULT_HZ = 1000
};

static int32_t s_led1;
static uint32_t s_output_pins;
static uint32_t s_pwm_pins;
static uint32_t s_irq_pins;
static uint16_t s_pwm_levels[22];
static uint32_t s_pwm_frequencies[8];
static uint8_t s_irq_masks[22];

static bool gpio_is_exposed(int32_t pin)
{
    return pin >= 0 && pin <= 21;  /* D0-D13 and A0-A7 */
}

extern "C" uint32_t yajir_irq_save(void)
{
    uint32_t state = __get_PRIMASK();
    __disable_irq();
    return state;
}

extern "C" void yajir_irq_restore(uint32_t state)
{
    __set_PRIMASK(state);
}

extern "C" void yajir_putc(char c)
{
    Serial.write((uint8_t)c);
}

extern "C" void yajir_puts(const char *s)
{
    if (!s) return;
    while (*s) yajir_putc(*s++);
}

extern "C" int32_t get_tick(void)
{
    return (int32_t)millis();
}

static void reg_out(const char *name, script_out_fn fn)
{
    script_register_out(name, fn);
    host_diag_note(name);
}

static void reg_in(const char *name, script_in_fn fn)
{
    script_register_in(name, fn, SCRIPT_T_INT);
    host_diag_note(name);
}

static void reg_inout(const char *name, script_in_fn get_fn,
                      script_out_fn set_fn)
{
    script_register_inout(name, get_fn, set_fn, SCRIPT_T_INT);
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

static int yajir_import(const char *name, const char **source,
                        uint32_t *length)
{
    for (int i = 0; i < YAJIR_NLIBS; ++i) {
        if (strcmp(name, YAJIR_LIBS[i].name) == 0) {
            *source = YAJIR_LIBS[i].source;
            *length = (uint32_t)strlen(YAJIR_LIBS[i].source);
            return 0;
        }
    }
    return -1;
}

static int32_t led1_get(void)
{
    return s_led1;
}

static void led1_set(int argc, const script_value_t *argv)
{
    s_led1 = argc > 0 && argv[0].i != 0;
    digitalWrite(LED_BUILTIN, s_led1 ? HIGH : LOW);
    script_set_result(s_led1);
}

static void rgb_led_write(int32_t red, int32_t green, int32_t blue)
{
    /* Nano R4's common-anode RGB LED is active-low. */
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    digitalWrite(LEDR, red ? LOW : HIGH);
    digitalWrite(LEDG, green ? LOW : HIGH);
    digitalWrite(LEDB, blue ? LOW : HIGH);
}

static void rgb_pwm_write(int32_t red, int32_t green, int32_t blue)
{
    /* Convert brightness to the active-low duty used by the onboard LED. */
    analogWrite(LEDR, YAJIR_PWM_MAX - red);
    analogWrite(LEDG, YAJIR_PWM_MAX - green);
    analogWrite(LEDB, YAJIR_PWM_MAX - blue);
}

static void th_rgb_led(int argc, const script_value_t *argv)
{
    int32_t red = argc > 0 && argv[0].i != 0;
    int32_t green = argc > 1 && argv[1].i != 0;
    int32_t blue = argc > 2 && argv[2].i != 0;
    rgb_led_write(red, green, blue);
}

static void th_rgb_pwm(int argc, const script_value_t *argv)
{
    int32_t red = argc > 0 ? argv[0].i : -1;
    int32_t green = argc > 1 ? argv[1].i : -1;
    int32_t blue = argc > 2 ? argv[2].i : -1;

    if (argc < 3 || red < 0 || red > YAJIR_PWM_MAX ||
        green < 0 || green > YAJIR_PWM_MAX ||
        blue < 0 || blue > YAJIR_PWM_MAX) {
        script_set_result(-1);
        return;
    }
    rgb_pwm_write(red, green, blue);
    script_set_result(0);
}

static void th_adc_get(int argc, const script_value_t *argv)
{
    int32_t channel = argc > 0 ? argv[0].i : -1;

    if (channel < 0 || channel > 7) {
        script_set_result(-1);
        return;
    }
    script_set_result((int32_t)analogRead((pin_size_t)(14 + channel)));
}

static void th_adc_pin(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;

    if (pin < 14 || pin > 21) {
        script_set_result(-1);
        return;
    }
    script_set_result((int32_t)analogRead((pin_size_t)pin));
}

static int pwm_channel(int32_t pin)
{
    if (!gpio_is_exposed(pin) || !digitalPinHasPWM(pin)) return -1;
    auto cfg = getPinCfgs((pin_size_t)pin, PIN_CFG_REQ_PWM);
    return cfg[0] == 0 ? -1 : (int)GET_CHANNEL(cfg[0]);
}

static void pwm_restore_channel(int channel)
{
    int32_t pin;

    for (pin = 0; pin <= 21; ++pin) {
        if ((s_pwm_pins & (1u << pin)) != 0 && pwm_channel(pin) == channel)
            analogWrite((pin_size_t)pin, s_pwm_levels[pin]);
    }
}

static bool pwm_set_frequency(int32_t pin, int32_t frequency)
{
    int channel = pwm_channel(pin);
    FspTimer *timer;

    if (channel < 0 || channel >= 8 || frequency <= 0) return false;
    if ((s_pwm_pins & (1u << pin)) == 0) {
        analogWrite((pin_size_t)pin, s_pwm_levels[pin]);
        s_pwm_pins |= 1u << pin;
    }

    timer = __get_timer_for_channel(channel);
    if (!timer->set_frequency((float)frequency)) return false;
    s_pwm_frequencies[channel] = (uint32_t)frequency;
    pwm_restore_channel(channel);
    return true;
}

static void th_pwm_set(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t level = argc > 1 ? argv[1].i : -1;
    int channel;

    if (argc < 2 || !gpio_is_exposed(pin) || !digitalPinHasPWM(pin) ||
        level < 0 || level > YAJIR_PWM_MAX) {
        script_set_result(-1);
        return;
    }

    channel = pwm_channel(pin);
    s_pwm_levels[pin] = (uint16_t)level;
    s_pwm_pins |= 1u << pin;
    analogWrite((pin_size_t)pin, level);
    if (s_pwm_frequencies[channel] == 0 &&
        !pwm_set_frequency(pin, YAJIR_PWM_DEFAULT_HZ)) {
        script_set_result(-1);
        return;
    }
    script_set_result(level);
}

static void th_pwm_get(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;

    if (!gpio_is_exposed(pin) || !digitalPinHasPWM(pin)) {
        script_set_result(-1);
        return;
    }
    script_set_result((int32_t)s_pwm_levels[pin]);
}

static void th_pwm_freq(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t frequency = argc > 1 ? argv[1].i : -1;

    if (argc < 2 || !pwm_set_frequency(pin, frequency)) {
        script_set_result(-1);
        return;
    }
    script_set_result(frequency);
}

static int gpio_irq_channel(int32_t pin)
{
    if (!gpio_is_exposed(pin)) return -1;
    auto cfg = getPinCfgs((pin_size_t)pin, PIN_CFG_REQ_INTERRUPT);
    return cfg[0] == 0 ? -1 : (int)GET_CHANNEL(cfg[0]);
}

static void gpio_irq_disable_pin(int32_t pin)
{
    if (pin < 0 || pin > 21 || (s_irq_pins & (1u << pin)) == 0) return;
    detachInterrupt((pin_size_t)pin);
    s_irq_pins &= ~(1u << pin);
    s_irq_masks[pin] = 0;
}

static void gpio_irq_callback(void *param)
{
    int32_t pin = (int32_t)(uintptr_t)param - 1;
    int32_t value;
    int32_t event;
    int32_t mask;
    script_arg_t args[3];

    if (pin < 0 || pin > 21) return;
    value = digitalRead((pin_size_t)pin) == HIGH ? 1 : 0;
    mask = s_irq_masks[pin];
    event = mask == YAJIR_GPIO_IRQ_LOW ? YAJIR_GPIO_IRQ_LOW :
            mask == YAJIR_GPIO_IRQ_RISE ? YAJIR_GPIO_IRQ_RISE :
            mask == YAJIR_GPIO_IRQ_FALL ? YAJIR_GPIO_IRQ_FALL :
            value ? YAJIR_GPIO_IRQ_RISE : YAJIR_GPIO_IRQ_FALL;

    args[0] = SCRIPT_ARG_INT(pin);
    args[1] = SCRIPT_ARG_INT(event);
    args[2] = SCRIPT_ARG_INT(value);
    script_post_msg_v("GPIO_IRQ", 3, args);
}

static void th_gpio_irq_enable(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t mask = argc > 1 ? argv[1].i : -1;
    int channel;
    int32_t other;
    PinStatus mode;

    if (argc < 2 || !gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    if (mask == 0) {
        gpio_irq_disable_pin(pin);
        script_set_result(0);
        return;
    }
    if (!((mask >= YAJIR_GPIO_IRQ_RISE &&
           mask <= (YAJIR_GPIO_IRQ_RISE | YAJIR_GPIO_IRQ_FALL)) ||
          mask == YAJIR_GPIO_IRQ_LOW)) {
        script_set_result(-1);
        return;
    }

    channel = gpio_irq_channel(pin);
    if (channel < 0) {
        script_set_result(-1);
        return;
    }
    for (other = 0; other <= 21; ++other) {
        if (other != pin && (s_irq_pins & (1u << other)) != 0 &&
            gpio_irq_channel(other) == channel) {
            script_set_result(-1);
            return;
        }
    }

    gpio_irq_disable_pin(pin);
    mode = mask == YAJIR_GPIO_IRQ_LOW ? LOW :
           mask == YAJIR_GPIO_IRQ_RISE ? RISING :
           mask == YAJIR_GPIO_IRQ_FALL ? FALLING : CHANGE;
    s_irq_masks[pin] = (uint8_t)mask;
    attachInterruptParam((pin_size_t)pin, gpio_irq_callback, mode,
                         (void *)(uintptr_t)(pin + 1));
    s_irq_pins |= 1u << pin;
    script_set_result(0);
}

static void th_gpio_irq_disable(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;

    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    gpio_irq_disable_pin(pin);
    script_set_result(0);
}

static void th_gpio_get(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    script_set_result(gpio_is_exposed(pin) ? (int32_t)digitalRead((pin_size_t)pin) : -1);
}

static void th_gpio_set(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t value = argc > 1 && argv[1].i != 0;
    if (!gpio_is_exposed(pin)) {
        script_set_result(-1);
        return;
    }
    digitalWrite((pin_size_t)pin, value ? HIGH : LOW);
    script_set_result(value);
}

static void th_gpio_mode(int argc, const script_value_t *argv)
{
    int32_t pin = argc > 0 ? argv[0].i : -1;
    int32_t mode = argc > 1 ? argv[1].i : YAJIR_GPIO_IN;
    if (argc < 2 || !gpio_is_exposed(pin) ||
        mode < YAJIR_GPIO_IN || mode > YAJIR_GPIO_IN_PULLDOWN) {
        script_set_result(-1);
        return;
    }

    /* ArduinoCore-renesas 1.6.0 does not implement INPUT_PULLDOWN. */
    if (mode == YAJIR_GPIO_IN_PULLDOWN) {
        script_set_result(-1);
        return;
    }

    gpio_irq_disable_pin(pin);

    switch (mode) {
        case YAJIR_GPIO_OUT:
            pinMode((pin_size_t)pin, OUTPUT);
            s_output_pins |= 1u << pin;
            break;
        case YAJIR_GPIO_IN_PULLUP:
            pinMode((pin_size_t)pin, INPUT_PULLUP);
            s_output_pins &= ~(1u << pin);
            break;
        default:
            pinMode((pin_size_t)pin, INPUT);
            s_output_pins &= ~(1u << pin);
            break;
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
    value = digitalRead((pin_size_t)pin) == LOW;
    digitalWrite((pin_size_t)pin, value ? HIGH : LOW);
    script_set_result(value);
}

extern "C" void host_register_all(void)
{
    host_diag_reset();

    reg_inout("LED1", led1_get, led1_set);
    reg_out("RGB_LED", th_rgb_led);
    reg_out("RGB_PWM", th_rgb_pwm);
    script_register_now(get_tick);
    reg_inout("ADC_GET", NULL, th_adc_get);
    reg_inout("ADC_PIN", NULL, th_adc_pin);
    reg_inout("PWM_SET", NULL, th_pwm_set);
    reg_inout("PWM_GET", NULL, th_pwm_get);
    reg_out("PWM_FREQ", th_pwm_freq);
    reg_inout("GPIO_GET", NULL, th_gpio_get);
    reg_inout("GPIO_SET", NULL, th_gpio_set);
    reg_out("GPIO_MODE", th_gpio_mode);
    reg_inout("GPIO_TOGGLE", NULL, th_gpio_toggle);
    reg_out("GPIO_IRQ_ENABLE", th_gpio_irq_enable);
    reg_out("GPIO_IRQ_DISABLE", th_gpio_irq_disable);
    script_register_stdout(yajir_puts);
    script_register_import(yajir_import);
    reg_handler("USB_SERIAL");
    reg_handler("GPIO_IRQ");

    reg_const("GPIO_IN", YAJIR_GPIO_IN);
    reg_const("GPIO_OUT", YAJIR_GPIO_OUT);
    reg_const("GPIO_IN_PULLUP", YAJIR_GPIO_IN_PULLUP);
    reg_const("GPIO_IN_PULLDOWN", YAJIR_GPIO_IN_PULLDOWN);
    reg_const("GPIO_IRQ_RISE", YAJIR_GPIO_IRQ_RISE);
    reg_const("GPIO_IRQ_FALL", YAJIR_GPIO_IRQ_FALL);
    reg_const("GPIO_IRQ_LOW", YAJIR_GPIO_IRQ_LOW);
    reg_const("ADC_MAX", YAJIR_ADC_MAX);
    reg_const("PWM_MAX", YAJIR_PWM_MAX);
    reg_const("PWM_DEFAULT_FREQ", YAJIR_PWM_DEFAULT_HZ);
}

extern "C" void yajir_glue_init(void)
{
    s_led1 = 0;
    s_output_pins = 0;
    s_pwm_pins = 0;
    s_irq_pins = 0;
    for (int pin = 0; pin <= 21; ++pin) {
        s_pwm_levels[pin] = 0;
        s_irq_masks[pin] = 0;
    }
    for (int channel = 0; channel < 8; ++channel)
        s_pwm_frequencies[channel] = 0;
    analogReadResolution(14);
    analogWriteResolution(16);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    rgb_led_write(0, 0, 0);
}

extern "C" void yajir_glue_stop(void)
{
    int32_t pin;

    for (pin = 0; pin <= 21; ++pin)
        gpio_irq_disable_pin(pin);
    for (pin = 0; pin <= 21; ++pin) {
        if ((s_pwm_pins & (1u << pin)) != 0) {
            analogWrite((pin_size_t)pin, 0);
            pinMode((pin_size_t)pin, OUTPUT);
            digitalWrite((pin_size_t)pin, LOW);
            s_pwm_levels[pin] = 0;
        }
    }
    s_pwm_pins = 0;
    for (pin = 0; pin < 8; ++pin)
        s_pwm_frequencies[pin] = 0;
    digitalWrite(LED_BUILTIN, LOW);
    rgb_led_write(0, 0, 0);
    s_led1 = 0;
    for (pin = 0; pin <= 21; ++pin) {
        if ((s_output_pins & (1u << pin)) != 0)
            digitalWrite((pin_size_t)pin, LOW);
    }
    s_output_pins = 0;
}
