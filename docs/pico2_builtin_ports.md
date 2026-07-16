# Raspberry Pi Pico 2 W 組込みポート

この文書では、YajirのRaspberry Pi Pico 2 Wホストが追加するポート、イベントハンドラ、定数を説明します。

対象は現在実装済みのUSB serial、オンボードLED、GPIO、GPIO IRQ、ADC、PWMです。UART、I2C、Wi-Fi、HTTPはまだ含みません。

## 共通仕様

外部端子として指定できるGPIO番号は`0..22`および`26..28`です。

引数不足、不正なピン番号、範囲外の設定値では、産出値または`RESULT`が`-1`になります。設定専用ポートは、正常終了時に原則として`RESULT`へ`0`、または実際に設定した値を格納します。

## 基本ポート

| ポート | 形式 | 内容 |
|---|---|---|
| `LED1` | `value -> LED1` | Pico 2 WオンボードLEDを制御。`0`で消灯、非0で点灯 |
| `LED1` | `LED1` | 最後に設定したオンボードLED値を取得 |
| `STDOUT` | `value... -> STDOUT` | 整数またはUTF-8文字列をUSB serialへ連結して出力し、改行 |
| `NOW` | `NOW` | 起動後の経過時間をミリ秒で取得 |
| `DELAY` | `ms -> DELAY` | 指定時間だけホスト処理をブロッキング |
| `VMSIZE` | `VMSIZE` | 現在のPico設定におけるVM arenaサイズをbyte単位で取得 |

`LED1`はCYW43チップ経由で制御されるオンボードLEDです。RP2350のGPIO PWM出力ではないため、`PWM_SET`の対象にはできません。

通常の待機には、ホスト全体を止めないYajir組込みポート`WAIT`を使用します。`DELAY`中はUSB入力処理も停止し、処理終了後に再開します。

```yajir
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
```

## USB Serial

USB CDC serialからスクリプトを投入します。スクリプト本文に続けて、単独行で`@run`を送るとコンパイルして実行します。

実行開始後に受信した各byteは、`USB_SERIAL`イベントとしてスクリプトへpostされます。

```yajir
ON USB_SERIAL
    ARG[0] -> STDOUT
END
```

| ハンドラ | 引数 | 内容 |
|---|---|---|
| `ON USB_SERIAL` | `ARG[0]` | 受信した1 byteの整数値 |

`Ctrl+C`（ASCII `0x03`）はローダが予約しています。実行中に入力するとスクリプトを強制中断し、新しいスクリプトの受信待ちへ戻ります。`Ctrl+C`自体は`ON USB_SERIAL`へpostされません。

強制中断時はGPIO IRQとPWMを停止します。PWMに使用していたピンはGPIO出力へ戻し、Lowを設定します。

## GPIO

### GPIOポート

| ポート | 形式 | 産出値／`RESULT` | 内容 |
|---|---|---:|---|
| `GPIO_GET` | `pin -> GPIO_GET` | `0`または`1` | GPIOの現在値を取得 |
| `GPIO_SET` | `pin, value -> GPIO_SET` | 設定した`0`または`1` | GPIOへ出力 |
| `GPIO_MODE` | `pin, mode -> GPIO_MODE` | 成功時`0` | 入出力モードとpullを設定 |
| `GPIO_TOGGLE` | `pin -> GPIO_TOGGLE` | 反転後の`0`または`1` | GPIOの出力ラッチを反転 |

`GPIO_SET`では`0`をLow、非0をHighとして扱います。返される値は正規化された`0`または`1`です。

### GPIOモード定数

| 定数 | 値 | 内容 |
|---|---:|---|
| `GPIO_IN` | 0 | 入力、pullなし |
| `GPIO_OUT` | 1 | 出力 |
| `GPIO_IN_PULLUP` | 2 | pull-up入力 |
| `GPIO_IN_PULLDOWN` | 3 | pull-down入力 |

```yajir
INIT
    15, GPIO_OUT -> GPIO_MODE
END

MAIN
    15 -> GPIO_TOGGLE
    500 -> WAIT
END
```

## GPIO IRQ

GPIOのエッジまたはレベル変化を、`ON GPIO_IRQ`へ非同期イベントとしてpostします。

| ポート | 形式 | `RESULT` | 内容 |
|---|---|---:|---|
| `GPIO_IRQ_ENABLE` | `pin, mask -> GPIO_IRQ_ENABLE` | 成功時`0` | 指定条件のGPIO IRQを有効化 |
| `GPIO_IRQ_DISABLE` | `pin -> GPIO_IRQ_DISABLE` | 成功時`0` | 指定GPIOのIRQをすべて無効化 |

### IRQマスク定数

| 定数 | 値 | 内容 |
|---|---:|---|
| `GPIO_IRQ_RISE` | 1 | 立ち上がりエッジ |
| `GPIO_IRQ_FALL` | 2 | 立ち下がりエッジ |
| `GPIO_IRQ_HIGH` | 4 | Highレベル |
| `GPIO_IRQ_LOW` | 8 | Lowレベル |

複数条件は定数を加算して指定できます。

```yajir
INIT
    15, GPIO_IN_PULLUP -> GPIO_MODE
    15, GPIO_IRQ_RISE + GPIO_IRQ_FALL -> GPIO_IRQ_ENABLE
END

ON GPIO_IRQ
    "pin=", ARG[0], " event=", ARG[1], " value=", ARG[2] -> STDOUT
END
```

| ハンドラ引数 | 内容 |
|---|---|
| `ARG[0]` | IRQが発生したGPIO番号 |
| `ARG[1]` | 発生したIRQ条件のマスク |
| `ARG[2]` | イベントpost時点のGPIO値、`0`または`1` |

レベルIRQは条件が成立している間、イベントが連続して発生する可能性があります。通常の入力検出には立ち上がり／立ち下がりエッジを推奨します。

## ADC

RP2350のADC値は12 bitの整数`0..4095`として取得します。

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `ADC_GET` | `channel -> ADC_GET` | `0..4095` | ADC channel `0..2`を取得 |
| `ADC_PIN` | `pin -> ADC_PIN` | `0..4095` | GPIO `26..28`を指定してADC値を取得 |
| `ADC_TEMP` | `ADC_TEMP` | 摂氏温度 x100 | RP2350内部温度の概算値 |

ADC channelとGPIOの対応は次の通りです。

| ADC channel | GPIO |
|---:|---:|
| 0 | 26 |
| 1 | 27 |
| 2 | 28 |

```yajir
MAIN
    0 -> ADC_GET -> VAR[0]
    "ADC0=", VAR[0] -> STDOUT
    "TEMP(C x100)=", ADC_TEMP -> STDOUT
    1000 -> WAIT
END
```

`ADC_TEMP`はチップ内部温度センサーの公称変換式を使った概算値です。校正済みの周囲温度計ではありません。

## PWM

PWMは外部GPIO `0..22`および`26..28`で使用できます。デューティ値は`0..65535`で指定します。

| ポート | 形式 | 産出値／`RESULT` | 内容 |
|---|---|---:|---|
| `PWM_SET` | `pin, level -> PWM_SET` | 設定した`level` | デューティ値を設定し、PWM出力を開始 |
| `PWM_GET` | `pin -> PWM_GET` | 最後に設定した`level` | デューティ設定値を取得 |
| `PWM_FREQ` | `pin, hz -> PWM_FREQ` | `RESULT`に実設定周波数 | PWM周波数を設定 |

### PWM定数

| 定数 | 値 | 内容 |
|---|---:|---|
| `PWM_MAX` | 65535 | デューティ値の最大値 |
| `PWM_DEFAULT_FREQ` | 1000 | 初回`PWM_SET`時の既定周波数、1 kHz |

`PWM_FREQ`を呼ばずに`PWM_SET`した場合は、1 kHzで初期化されます。指定周波数はRP2350のPWMクロック分周とwrap値へ変換されるため、実際の周波数には丸めが入る場合があります。実設定値は`RESULT`で確認できます。

RP2350では一つのPWM sliceにA/Bの2 channelがあり、両channelは周波数を共有します。同じsliceに属する一方のピンで`PWM_FREQ`を変更すると、もう一方のchannelにも同じ周波数が適用されます。周波数変更時も、それぞれに設定済みのデューティ比は維持されます。

```yajir
def_alias(PWM_PIN, 15)

INIT
    PWM_PIN, 1000 -> PWM_FREQ
END

MAIN
    PWM_PIN, 32768 -> PWM_SET
    1000 -> WAIT
    PWM_PIN, 0 -> PWM_SET
    1000 -> WAIT
END
```

## サンプルスクリプト

| ファイル | 内容 |
|---|---|
| `scripts/pico2_w/blink.yaj` | オンボードLED点滅 |
| `scripts/pico2_w/gpio_chain.yaj` | スクリプト内ポートによる固定GPIOチェイン |
| `scripts/pico2_w/gpio_irq.yaj` | GPIO 15のpull-up入力と両エッジIRQ |
| `scripts/pico2_w/adc_temp.yaj` | RP2350内部温度表示 |
| `scripts/pico2_w/pwm_fade.yaj` | GPIO 15に接続したLEDのPWMフェード |
| `scripts/pico2_w/utility_ports.yaj` | 方向付きGPIO26とPWMブザー |
| `scripts/pico2_w/usb_morse.yaj` | USB入力をオンボードLEDでモールス送信 |
| `scripts/pico2_w/usb_morse_handler.yaj` | ハンドラ駆動で実装したモールス送信 |

## 現在のメモリ構成

現在のPico 2 W設定では、Yajir core、コンパイラ静的領域、16 KiBのスクリプト受信バッファ、PicoホストのYajir用静的状態を合計して`122,723 bytes`（約`119.85 KiB`）です。

この値にはPico SDK、CYW43、ヒープ、Cスタックを含みません。
