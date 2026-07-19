/* nano_r4.ino - Yajir firmware entry point for Arduino Nano R4 */
#include "yajir_build_config.h"
#include <Arduino.h>
#include "yajir_glue.h"
#include "yajir_loader.h"

void setup()
{
    uint32_t started;

    Serial.begin(115200);
    started = millis();
    while (!Serial && (uint32_t)(millis() - started) < 1500u) delay(1);

    yajir_glue_init();
    yajir_loader_init();
}

void loop()
{
    while (Serial.available() > 0)
        yajir_loader_feed_byte((uint8_t)Serial.read());

    yajir_loader_tick();
    delay(1);
}
