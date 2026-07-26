/* cardputer.ino - Yajir firmware entry point for M5Stack Cardputer */
#include "yajir_build_config.h"
#include "yajir_config_cardputer.h"
#include <Arduino.h>
#include <M5Cardputer.h>
#include "yajir_glue.h"
#include "yajir_loader.h"

void setup()
{
    uint32_t started;

    Serial.setRxBufferSize(YAJIR_SOURCE_BUFFER_SIZE);
    Serial.setTxBufferSize(2048);
    Serial.begin(115200);
    started = millis();
    while (!Serial && (uint32_t)(millis() - started) < 1500u) delay(1);

    auto config = M5.config();
    M5Cardputer.begin(config);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(2);
    M5Cardputer.Display.setTextColor(TFT_GREEN, TFT_BLACK);
    M5Cardputer.Display.setTextWrap(true);
    M5Cardputer.Display.setTextScroll(true);
    M5Cardputer.Display.clear(TFT_BLACK);
    M5Cardputer.Display.setCursor(0, 0);

    yajir_glue_init();
    yajir_loader_init();
}

void loop()
{
    M5Cardputer.update();

    while (Serial.available() > 0)
        yajir_loader_feed_byte((uint8_t)Serial.read());

    yajir_loader_tick();
    delay(1);
}
