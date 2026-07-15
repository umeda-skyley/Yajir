# Yajir Pico 2 W 移植計画

対象: Raspberry Pi Pico 2 W / Pico SDK(C/C++) / Yajir v0.4.5 以降

この計画は、Yajir core を Pico 2 W に載せ、Pico 2 W アプリを USB シリアルから投入する
スクリプトで作れるようにするための移植仕様メモです。最初の到達点は LED チカです。
Wi-Fi / HTTP は次段階に回し、まずはローカル I/O とローダを安定させます。

## 方針

- Pico SDK(C/C++) でビルドする。VS Code の Raspberry Pi Pico 拡張環境を前提にする。
- Yajir core は現行の `src/core` をそのまま使い、Pico2 W 固有部は `src/host/pico2_w` に分離する。
- `YJ_CONFIG_HEADER` と `YJ_PORT_HEADER` を使い、Pico2 W 専用の設定値とクリティカルセクションを差し込む。
- 第一段階の入出力は USB CDC serial。UART実ピンはペリフェラルポートとして後から扱う。
- Pico 2 W のオンボードLEDは無線チップ側の `WL_GPIO0` 接続なので、`LED1` は `cyw43_arch_gpio_put` 系で実装する。
- SRAMに余裕があるため、Pico2 W設定では core 既定値より大きめに確保する。

## 参考ハードウェア前提

- RP2350 / dual-core Arm Cortex-M33 または Hazard3 RISC-V
- 520KB SRAM
- USB 1.1 device/host
- 4MB onboard flash
- 26 multifunction GPIO
- 2 UART / 2 I2C / 2 SPI
- 3 ADC / 16 PWM channels
- Pico 2 W は Wi-Fi/Bluetooth 搭載。ただし今回の初期移植では未実装。

## ディレクトリ案

```text
src/host/pico2_w/
  CMakeLists.txt
  pico_sdk_import.cmake          # 置くか、SDK標準の外部参照にするかは環境に合わせる
  yajir_config_pico2_w.h
  yajir_port_pico2_w.h
  yajir_glue.h
  yajir_glue.c
  yajir_loader.h
  yajir_loader.c
  main.c
```

トップレベルに Pico 用 CMake を置く場合は、既存のPC/MSVCビルドを壊さないように
`pico/` または `src/host/pico2_w/` 以下だけで完結させる方針にします。

## Pico2 W config 初期値

Pico2 W はPCほどではないが、STM32版より大幅に広く取れます。最初は余裕寄りにします。

```c
#define CFG_GVAR_COUNT          32
#define CFG_VAR_COUNT           32
#define CFG_ARG_COUNT            8
#define CFG_SGVAR_COUNT         16
#define CFG_SVAR_COUNT          16
#define CFG_SARG_COUNT           8
#define CFG_SSTR_LEN          1024
#define CFG_SARG_LEN          1024
#define CFG_MAX_PORTS          160
#define CFG_MAX_BLOCKS          48
#define CFG_MAX_RESOURCES       64
#define CFG_MAX_ALIAS           64
#define CFG_STACK_DEPTH         64
#define CFG_NEST_LIMIT           8
#define CFG_LOOP_NEST            8
#define CFG_CALL_NEST            8
#define CFG_MAX_SCRIPT_PORTS    32
#define CFG_INSTR_BUDGET     50000
#define CFG_EVENT_QUEUE_LEN     16
#define CFG_TIMER_SLOTS         16
#define CFG_CODE_SIZE        16384
#define CFG_STRPOOL_SIZE     16384
#define CFG_MAX_NAME            32
```

`event_t` はイベントごとに `CFG_SARG_COUNT * CFG_SARG_LEN` の文字列領域を持つため、
キュー長64では文字列領域だけで512KBになり、RP2350のSRAMに収まりません。文字列長1KBを
維持したまま、初期キュー長は16とします。実測でarenaサイズを表示し、LEDチカ＋
ペリフェラルサンプルの余裕を見てから再調整します。

## クリティカルセクション

Pico SDK の割り込み制御で `YJ_ENTER_CRITICAL` / `YJ_EXIT_CRITICAL` を実装します。
STM32版と同じく、イベントキューの enqueue だけを短く保護します。

候補:

```c
#include "hardware/sync.h"

#define YJ_ENTER_CRITICAL()  uint32_t _yj_irq = save_and_disable_interrupts()
#define YJ_EXIT_CRITICAL()   restore_interrupts(_yj_irq)
```

## 第一段階: USBシリアル投入ローダ

STM32版 `yajir_loader.c` をベースにする。ただし入出力は Pico SDK の stdio USB。

### 起動時

- `stdio_init_all()`
- USB serial接続待ちは長く待ちすぎない。初期は `sleep_ms(1500)` 程度。
- `cyw43_arch_init()` を呼び、オンボードLED制御を可能にする。
- `script_init(arena)` と `host_register_all()` は、スクリプトロード時に毎回やり直す。

### 投入方式

第一段階はSTM32版と同じセンチネル方式:

```text
<script body>
@run
```

- 受信中は `g_src` に蓄積。
- `@run` でコンパイルして実行開始。
- `@fin` は「蓄積だけして実行しない」扱いとして残してもよい。
- 実行中に来た1文字は `USB_SERIAL` ハンドラへ post する。

### 最初に動かすスクリプト

```yaj
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
```

## 第二段階: 簡易シェル

USB serial 上に Yajir shell を用意する。最初は「開発時に便利な最小CLI」に絞る。

候補コマンド:

```text
help
info
new <name>
append
save
list
load <name>
run <name>
erase <name>
cat <name>
reset
```

第一実装では RAM 上だけでもよいが、次にフラッシュ保存へ進む。

### フラッシュ保存方針

LittleFS等は初期導入しない。まずは独自の固定スロット方式にする。

- flash末尾に Yajir script store 領域を確保する。
- 例: 256KBを予約し、4KBスロット x 64本、または 8KBスロット x 32本。
- 各スロットに header `{magic, version, name, length, crc}` + body。
- 保存時は該当スロットを sector erase して再書き込み。
- 実行中は flash erase/write しない。shell状態のときのみ保存操作を許可。

## 初期ポート一覧

既存のYajir流儀に合わせ、まず「使いやすい高レベルポート」を用意します。
ピン番号を引数で渡す汎用ポートと、よく使う固定名ポートを併用します。

### 基本

| ポート | 種別 | 産出 | 内容 |
|---|---|---|---|
| `STDOUT` | out | none | USB serialへ出力 |
| `NOW` | in | int | `time_us_64()/1000` 相当のms |
| `DELAY` | out | none | `sleep_ms`。ブロッキング |
| `VMSIZE` | in | int | arenaサイズ |
| `LED1` | inout | int | Pico 2 WオンボードLED。WL_GPIO0 |
| `ADC_TEMP` | in | int | 内蔵温度の概算値。摂氏 x100 |

### GPIO / ADC / PWM

| ポート | 種別 | 産出 | 内容 |
|---|---|---|---|
| `GPIO_GET` | inout | int | `pin -> GPIO_GET`。指定GPIOを読む |
| `GPIO_SET` | inout | int | `pin, value -> GPIO_SET`。指定GPIOを書き、書いた値を産出 |
| `GPIO_MODE` | out | none | `pin, mode -> GPIO_MODE`。mode=0 input, 1 output, 2 input_pullup, 3 input_pulldown |
| `GPIO_TOGGLE` | inout | int | `pin -> GPIO_TOGGLE`。反転後の値を産出 |
| `ADC_GET` | inout | int | `ch -> ADC_GET`。ADCチャンネル0..2を読む。0..4095 |
| `ADC_PIN` | inout | int | `pin -> ADC_PIN`。GPIO26..28をADCとして読む。0..4095 |
| `PWM_SET` | inout | int | `pin, level -> PWM_SET`。level=0..65535 duty。設定値を産出 |
| `PWM_GET` | inout | int | `pin -> PWM_GET`。最後に設定したdutyを読む |
| `PWM_FREQ` | out | none | `pin, hz -> PWM_FREQ`。同一PWM sliceのA/Bは周波数を共有 |

### GPIO IRQ

| ポート | 種別 | 産出 | 内容 |
|---|---|---|---|
| `GPIO_IRQ_ENABLE` | out | none | `pin, edge_mask -> GPIO_IRQ_ENABLE`。指定edgeで割り込み有効化 |
| `GPIO_IRQ_DISABLE` | out | none | `pin -> GPIO_IRQ_DISABLE`。指定GPIOの割り込みを無効化 |

GPIO IRQのedge maskは定数で用意する。

| 定数 | 値 | 内容 |
|---|---:|---|
| `GPIO_IRQ_RISE` | 1 | 立ち上がり |
| `GPIO_IRQ_FALL` | 2 | 立ち下がり |
| `GPIO_IRQ_HIGH` | 4 | High level |
| `GPIO_IRQ_LOW` | 8 | Low level |

ハンドラ `ON GPIO_IRQ` では `ARG[0]=pin`, `ARG[1]=edge_mask`, `ARG[2]=value` を渡す。

### UART / I2C

初期実装では UART は文字送信 + 受信イベント、I2C はよく使う 8bit register read/write を優先する。

| ポート | 方針 |
|---|---|
| `UART0_OPEN`, `UART1_OPEN` | `tx_pin, rx_pin, baud -> UARTx_OPEN`。UART初期化 |
| `UART0_PUTC`, `UART1_PUTC` | `ch -> UARTx_PUTC`。1文字送信。`'A'` や `ARG[0]` を渡す |
| `UART0_WRITE`, `UART1_WRITE` | `str -> UARTx_WRITE`。文字列送信 |
| `I2C0_OPEN`, `I2C1_OPEN` | `sda_pin, scl_pin, hz -> I2Cx_OPEN`。I2C初期化 |
| `I2C0_READ8`, `I2C1_READ8` | `addr, reg -> I2Cx_READ8`。8bit registerを読む |
| `I2C0_WRITE8`, `I2C1_WRITE8` | `addr, reg, value -> I2Cx_WRITE8`。8bit registerへ書く |

UART受信はポーリングではなく、ホスト側が1文字ごとにハンドラへpostする。
`UARTx_GETC` / `UARTx_AVAIL` は当面入れず、必要になったら後から追加する。

### イベント源

| ハンドラ | 内容 |
|---|---|
| `USB_SERIAL` | USB serial実行中入力。`ARG[0]` に1文字 |
| `UART0` | 実UART0受信。`ARG[0]` に1文字 |
| `UART1` | 実UART1受信。`ARG[0]` に1文字 |
| `BTN` | BOOTSELは通常GPIOとして読みにくいので初期は未実装でもよい |
| `GPIO_IRQ` | GPIO割り込み。`ARG[0]=pin`, `ARG[1]=edge_mask`, `ARG[2]=value` |
| `TIMER` | core標準の `TIMER` |

`STDOUT` は第一段階では USB serial へバインドする。実UARTへの出力は `UART0_PUTC` /
`UART0_WRITE` / `UART1_PUTC` / `UART1_WRITE` を使う。

## Wi-Fi / HTTP

初期移植では未実装。

後段で `pico_cyw43_arch_lwip_*` と lwIP を使い、PC版HTTPの小型版を検討する。
AI APIポートはPico2 W単体ではTLS/メモリ/証明書/レスポンスサイズの負荷が大きいため、
まずはHTTP GET/POSTの最小実装、またはPC/ローカルゲートウェイ経由を候補にする。

## 実装フェーズ

### Phase 0: プロジェクト骨格

- `src/host/pico2_w` を追加。
- Pico SDK CMakeで core + common + pico2_w host をビルド。
- `PICO_BOARD=pico2_w` を前提にする。
- `pico_stdlib`, `pico_cyw43_arch_none` をリンク。
- UF2生成まで確認。

### Phase 1: LEDチカ

- USB serialローダ。
- `STDOUT`, `NOW`, `DELAY`, `LED1`, `VMSIZE`。
- `@run` 投入でLEDチカ成功。
- `VERSION`, arena size, did-you-mean エラー表示。

### Phase 2: 基本GPIO

- `GPIO_GET`, `GPIO_SET`, `GPIO_MODE`, `GPIO_TOGGLE`。
- `ADC_GET`, `ADC_PIN`, `ADC_TEMP`, `PWM_SET`, `PWM_GET`, `PWM_FREQ` の最小実装。
- サンプル `scripts/pico2_w/blink.yaj`, `adc_pwm.yaj`。

### Phase 3: Shell

- USB serial shell。
- `new/append/save/list/load/run/cat/erase`。
- まずRAM、次にflash固定スロット保存。

### Phase 4: GPIO IRQ / UART / I2C

- GPIO IRQ。
- UART0/1の実ピン送信と受信イベント。
- I2C0/1。
- サンプル `gpio_irq.yaj`, `uart_echo.yaj`, `i2c_whoami.yaj`。

### Phase 5: Wi-Fi / HTTP

- Wi-Fi接続ポート。
- HTTP GET/POST。
- async HTTP。
- 必要ならローカルゲートウェイ方式でAI連携。

## 未決事項

- Shellの保存形式: 固定スロット方式か、小さなログ構造か。
- `ADC_TEMP` は専用ポートとして扱う。戻り値は摂氏 x100 の int とする。
- Pico2 WでのPC版 `FILE_READER/FILE_WRITER` 相当を、shell保存領域に接続するか。

## 非同期ハンドラ方針

初期実装で非同期ハンドラにするのは、外部から勝手に発生する入力だけに絞る。

| ハンドラ | 理由 |
|---|---|
| `USB_SERIAL` | USB serial入力はホスト側に届くタイミングが外部依存 |
| `UART0` / `UART1` | 実UART受信は外部依存。1文字ごとにpost |
| `GPIO_IRQ` | ピン変化は外部イベント |
| `TIMER` | core標準の非同期イベント |

ADC / I2C / PWM / GPIO_GET は、スクリプトが必要なタイミングで読む・書くポーリング型にする。
I2Cはデバイス側の割り込みピンがある場合、`GPIO_IRQ` と組み合わせてから `I2C*_READ8` する。

## 後日のお楽しみ枠

- SPI0/1。
- UART/I2C/SPIの文字列・バイト列転送。
- PIO連携。
- NeoPixelなど、よく使う外部デバイス向け高レベルポート。

## 参考

- Raspberry Pi Pico-series documentation: https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html
- Raspberry Pi Pico SDK documentation: https://www.raspberrypi.com/documentation/pico-sdk/
