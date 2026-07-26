# Yajir host for Raspberry Pi Pico 2 W

Pico SDK C/C++でYajirをRaspberry Pi Pico 2 Wへ移植するホスト実装です。
通常動作中は、USB CDCシリアルと256 KiBのUSBマスストレージ
`YAJIR`を同時に提供します。

## ビルド

### VS Code

VS CodeのRaspberry Pi Pico拡張から、このディレクトリをPicoプロジェクトとして
開きます。ボードには`pico2_w`を指定してください。

### コマンドライン

Pico SDKのパスを指定し、リポジトリのルートからビルドします。

```powershell
cmake -S src/host/pico2_w -B build/pico2_w `
    -DPICO_BOARD=pico2_w `
    -DPICO_SDK_PATH=C:/path/to/pico-sdk
cmake --build build/pico2_w
```

生成された`build/pico2_w/yajir_pico2_w.uf2`を、BOOTSELモードの
Pico 2 Wへ書き込みます。

ファームウェアは4 MiBフラッシュの末尾256 KiBを`YAJIR`ドライブ用に予約します。
この領域を守るため、CMakeのファームウェア領域は3840 KiBに制限されています。

## YAJIRスクリプトドライブ

ファームウェアの起動後、WindowsにはUSBシリアルポートと`YAJIR`ドライブが
表示されます。これはBOOTSEL時に見える`RPI-RP2`ドライブとは別の、通常動作中の
スクリプト用ドライブです。

1. Windowsで`YAJIR`ドライブを開きます。
2. 実行したいスクリプトをルートへ`autorun.yaj`という名前で保存します。
3. Windowsの「取り出し」を実行して書き込みを完了させます。
4. Pico 2 Wを再接続または再起動します。

起動時に`autorun.yaj`が見つかると、自動的にロード、コンパイル、実行します。

```text
[autorun] script received: ... bytes
[autorun] running. USB input is now sent to ON USB_SERIAL.
```

ファイルがない場合、14 KiBを超える場合、またはロードに失敗した場合は、
USBシリアルからのスクリプト投入待ちへ戻ります。Yajirは起動時だけ
`autorun.yaj`を読むため、ドライブ上のファイルを変更した後は再起動が必要です。

### スクリプトライブラリ

v0.4.10の`def_import`では、YAJIRドライブのルートに置いた8.3形式のファイルを
ライブラリとして取り込めます。

```yajir
def_import("utilport")
```

この宣言は`UTILPORT.YAJ`を読み込みます。指定する名前は拡張子を除く1～8文字で、
英数字、`_`、`-`が使用でき、大文字と小文字は区別しません。サブディレクトリと
ロングファイル名には対応していません。ライブラリは最大4095 bytesで、同時に保持せず
一つずつコンパイルするため、複数のimportでも同じバッファを再利用します。

読込バッファには既存の4 KiBフラッシュ書込みキャッシュを一時利用します。このため
`def_import`対応によるPicoホストの追加RAM消費はありません。ライブラリ内のエラーには、
指定したライブラリ名とそのファイル内の行番号が表示されます。

同梱デモは`scripts/lib/blinker.yaj`を`BLINKER.YAJ`、`scripts/import_demo.yaj`を
`AUTORUN.YAJ`としてYAJIRドライブへ置くと試せます。

Pico向けの選択式ライブラリは`scripts/lib/pico2_w/`にあります。Windows上では必要な
ファイルを`gpio.yaj`、`adc.yaj`、`pwm.yaj`という小文字名のままYAJIRドライブへ置けます。
組込みポートを用途別のスクリプト内ポートへまとめているため、必要なものだけimportできます。
`gpio.yaj`はGPIO26～28の立ち下がりIRQもピン別に再配送します。例えば
`1 -> _GPIO26_IRQ`で有効化すると、アプリは引数判定なしの`ON GPIO26_IRQ`で受信できます。

FNK0089車体向けには`scripts/lib/fnk0089/`に、モーター、ブザー、ラインセンサー、
超音波センサー、16x8ドットマトリックス、5x7英字フォントの選択式ライブラリがあります。

## USBシリアルからの投入

`autorun.yaj`がない場合は、USBシリアル端末に次のプロンプトを表示します。

```text
==== Yajir on Raspberry Pi Pico 2 W ====
Version: ...
VM arena: ... bytes
[autorun] autorun.yaj not found.
Paste a script, then enter @run on its own line.
>
```

受信待ち中は、Pico 2 Wが受け取った文字をUSBシリアルへエコーバックします。
実行開始後も、入力文字を表示しながら同じbyteを`ON USB_SERIAL`へpostします。
端末ソフト側のローカルエコーは無効にしてください。

スクリプト本文を貼り付け、その直後に単独行で`@run`を送ります。

```text
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
@run
```

実行開始後のUSB入力は、1 byteずつ`ON USB_SERIAL`へpostされます。
受信したbyteの整数値は`ARG[0]`に入ります。

```yajir
ON USB_SERIAL
    ARG[0] -> STDOUT
END
```

## Ctrl+Cによる中断

実行中に`Ctrl+C`（ASCII `0x03`）を送ると、スクリプトを強制中断して
USBシリアルからの投入待ちへ戻ります。

```text
^C
[loader] script stopped.
Paste a script, then enter @run on its own line.
>
```

中断時はGPIO IRQとPWMを停止し、PWMに使用していたピンをLowへ戻します。
`Ctrl+C`はローダー専用の制御文字であり、`ON USB_SERIAL`にはpostされません。
ブロッキングする`DELAY`の実行中は、処理がUSBポーリングへ戻った時点で
中断されます。通常の`WAIT`中はすぐに反応します。

## 組込みポート

### 共通仕様

外部端子として指定できるGPIO番号は`0..22`および`26..28`です。

引数不足、不正なピン番号、範囲外の設定値では、産出値または`RESULT`が`-1`になります。
設定専用ポートは、正常終了時に原則として`RESULT`へ`0`、または実際に設定した値を格納します。

### 基本ポート

| ポート | 形式 | 内容 |
|---|---|---|
| `LED1` | `value -> LED1` | Pico 2 WオンボードLEDを制御。`0`で消灯、非0で点灯 |
| `LED1` | `LED1` | 最後に設定したオンボードLED値を取得 |
| `STDOUT` | `value... -> STDOUT` | 整数またはUTF-8文字列をUSB serialへ連結して出力し、改行 |
| `NOW` | `NOW` | 起動後の経過時間をミリ秒で取得 |
| `DELAY` | `ms -> DELAY` | 指定時間だけホスト処理をブロッキング |
| `SLEEP` | `ms -> SLEEP` | 最大`ms`のSleep。タイマーまたは任意の有効IRQで復帰 |

`LED1`はCYW43チップ経由で制御されるオンボードLEDです。RP2350のGPIO PWM出力ではないため、
`PWM_SET`の対象にはできません。

通常の待機には、ホスト全体を止めないYajir組込みポート`WAIT`を使用します。
`DELAY`中はUSB入力処理も停止し、処理終了後に再開します。

`SLEEP`はRP2350の低電力Sleepへ入り、復帰後は次の行から実行を再開します。
`ms > 0`は最大待機時間で、GPIO IRQやUSB CDC受信など別の有効な割り込みでも早期復帰します。
`ms <= 0`は割り込みが発生するまで待機します。GPIOを起床源にする場合は、先に
`GPIO_IRQ_ENABLE`で設定します。

```yajir
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
```

### USB Serialイベント

実行開始後にUSB CDC serialから受信した各byteは、`USB_SERIAL`イベントとして
スクリプトへpostされます。

| ハンドラ | 引数 | 内容 |
|---|---|---|
| `ON USB_SERIAL` | `ARG[0]` | 受信した1 byteの整数値 |

`Ctrl+C`（ASCII `0x03`）はローダーが予約しているため、`ON USB_SERIAL`へはpostされません。

## GPIO

### GPIOポート

| ポート | 形式 | 産出値／`RESULT` | 内容 |
|---|---|---:|---|
| `GPIO_GET` | `pin -> GPIO_GET` | `0`または`1` | GPIOの現在値を取得 |
| `GPIO_SET` | `pin, value -> GPIO_SET` | 設定した`0`または`1` | GPIOへ出力 |
| `GPIO_MODE` | `pin, mode -> GPIO_MODE` | 成功時`0` | 入出力モードとpullを設定 |
| `GPIO_TOGGLE` | `pin -> GPIO_TOGGLE` | 反転後の`0`または`1` | GPIOの出力ラッチを反転 |
| `GPIO_PULSE` | `pin, level, width_us -> GPIO_PULSE` | 指定した`width_us` | 反対レベルから指定レベルのパルスを出力 |
| `PULSE_IN` | `pin, level, timeout_us -> PULSE_IN` | パルス幅、timeout時`0` | 指定レベルのパルス幅をマイクロ秒で計測 |

`GPIO_SET`では`0`をLow、非0をHighとして扱います。返される値は正規化された`0`または`1`です。
`GPIO_PULSE`と`PULSE_IN`の時間指定は`1..10000000` usです。どちらも同期処理のため、
実行中は指定時間または入力パルス終了までホスト処理をブロックします。`PULSE_IN`は
計測開始前から同じレベルだった場合、いったんそのパルスが終わってから次のパルスを待ちます。

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

レベルIRQは条件が成立している間、イベントが連続して発生する可能性があります。
通常の入力検出には立ち上がり／立ち下がりエッジを推奨します。

## ADC

RP2350のADC値は12 bitの整数`0..4095`として取得します。定数`ADC_MAX`も`4095`です。

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

`ADC_TEMP`はチップ内部温度センサーの公称変換式を使った概算値です。
校正済みの周囲温度計ではありません。

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

`PWM_FREQ`を呼ばずに`PWM_SET`した場合は、1 kHzで初期化されます。
指定周波数はRP2350のPWMクロック分周とwrap値へ変換されるため、実際の周波数には丸めが
入る場合があります。実設定値は`RESULT`で確認できます。

RP2350では一つのPWM sliceにA/Bの2 channelがあり、両channelは周波数を共有します。
同じsliceに属する一方のピンで`PWM_FREQ`を変更すると、もう一方のchannelにも同じ周波数が
適用されます。周波数変更時も、それぞれに設定済みのデューティ比は維持されます。

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

## I2C

I2C0をマスターとして使用します。`I2C0_OPEN`で初期化してから読み書きしてください。

| ポート | 形式 | 産出値／`RESULT` | 内容 |
|---|---|---:|---|
| `I2C0_OPEN` | `sda, scl, hz -> I2C0_OPEN` | 実設定周波数 | I2C0を初期化 |
| `I2C0_WRITE` | `addr, byte... -> I2C0_WRITE` | 送信byte数、失敗時は負値 | 1～7 byteを一回の転送で送信 |
| `I2C0_WRITE8` | `addr, reg, value -> I2C0_WRITE8` | 成功時`2`、失敗時は負値 | 8 bitレジスタへ1 byte書込み |
| `I2C0_READ8` | `addr, reg -> I2C0_READ8` | 読出値`0..255`、失敗時`-1` | repeated-startで8 bitレジスタを読出し |

I2C0の有効なピン組は、外部へ公開されたGPIOのうちSDAが`0 mod 4`、SCLがその次の
GPIOとなる組です。代表例はGPIO4/5です。周波数は`1000..1000000` Hz、7 bitアドレスは
`0x08..0x77`を受け付けます。一回のI2C処理は20 msでタイムアウトします。

```yajir
INIT
    4, 5, 100000 -> I2C0_OPEN
    0x71, 0x21 -> I2C0_WRITE
END
```

`Ctrl+C`でスクリプトを停止するとI2C0をdeinitし、SDA/SCLをpullなしのGPIO入力へ戻します。

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
| `scripts/pico2_w/usb_morse_selfdrive.yaj` | `AFTER`で自己駆動するイベントチェイン型のモールス送信 |
| `scripts/pico2_w/sleep_wakeup.yaj` | タイマーまたはUSB/GPIO IRQで復帰するSleep |
| `scripts/pico2_w/lib_demo.yaj` | GPIO、ADC、PWMライブラリの選択import |
| `scripts/pico2_w/lib_gpio_irq.yaj` | GPIOライブラリによるGPIO26のピン別falling IRQ |
| `scripts/fnk0089/*.yaj` | FNK0089のモーター、ブザー、ライン、超音波、マトリックス確認 |

## 現在のメモリ構成

現在のPico 2 W設定では、Yajir core、コンパイラ静的領域、14 KiBのスクリプト受信バッファ、
PicoホストのYajir用静的状態を合計して`130,166 bytes`（約`127.12 KiB`）です。
v0.4.7の遅延postは`CFG_DELAY_SLOTS=2`とし、同時に2本まで待機できます。
v0.4.10の取り込み済み名前表は`CFG_MAX_IMPORTS=8`です。

この値にはPico SDK、CYW43、ヒープ、Cスタックを含みません。

## 関連資料

- [Pico 2 Wサンプルスクリプト](../../../scripts/pico2_w)
- [FNK0089ライブラリ](../../../scripts/lib/fnk0089)
- [FNK0089実機サンプル](../../../scripts/fnk0089)
