# Pico 2 Wスクリプトライブラリ

YAJIRドライブのルートへ必要な`.yaj`だけを置き、アプリの最上部から選択的に
`def_import`します。Windows上ではファイル名を小文字で置いて構いません。

| ファイル | import | 内容 |
|---|---|---|
| `gpio.yaj` | `def_import("gpio")` | GPIO26～28の方向付き入出力ポート |
| `adc.yaj` | `def_import("adc")` | ADC0～2、mV、百分率、内部温度 |
| `pwm.yaj` | `def_import("pwm")` | 百分率PWM、周波数、サーボ、トーン |

各ファイルは4 KiB未満で、`INIT`、`MAIN`、グローバル変数を持ちません。
importしただけではGPIOやPWMの状態を変更しません。

## GPIO

アンダースコアは矢印を接続できる側を表します。

```yajir
GPIO26_ -> STDOUT
1 -> _GPIO27
0 -> _GPIO27_ -> STDOUT
```

IRQはGPIO26～28の立ち下がり専用です。有効化すると入力モードはpull-upになります。
ライブラリが生の`ON GPIO_IRQ`を受け、ピン別のハンドラへ引数なしで再配送します。

```yajir
INIT
    1 -> _GPIO26_IRQ
END

ON GPIO26_IRQ
    NOT LED1 -> LED1
END
```

`0 -> _GPIO26_IRQ`で無効化します。`GPIO27_IRQ`と`GPIO28_IRQ`も同じ要領で使えます。
アプリ側では`def_handler`の宣言や`ARG`の判定は不要です。

## ADC

```yajir
ADC0_ -> STDOUT
26 -> ADC_MV_ -> STDOUT
26 -> ADC_PCT_ -> STDOUT
TEMP_C_ -> STDOUT
```

`ADC_MV_`は基準電圧を3300mVとして整数換算します。`TEMP_C_`は摂氏100倍です。

## PWM

```yajir
15, 50 -> _PWM_PCT
15 -> PWM_PCT_ -> STDOUT
15, 1000 -> _PWM_HZ_ -> STDOUT
15, 90 -> _SERVO_
15, 440 -> _TONE_
15, 0 -> _TONE_
```

`_SERVO_`は50Hz、パルス幅約0.5～2.5ms相当です。同じPWM sliceを共有するピンでは
周波数も共有されるため、サーボとトーンなど異なる周波数の用途を同じsliceへ割り当てないでください。
