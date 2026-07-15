# Yajir host for Raspberry Pi Pico 2 W

Pico SDK C/C++ でYajir v0.4.6をPico 2 Wへ載せるホストです。USB CDC serialから
スクリプトを投入し、オンボードLED、GPIO、GPIO IRQ、ADCを制御できます。Wi-FiとHTTPはまだ含みません。

## ビルド

VS CodeのRaspberry Pi Pico拡張からこのディレクトリをPicoプロジェクトとして開くか、Pico SDKの
パスを指定してコマンドラインからビルドします。

```powershell
cmake -S src/host/pico2_w -B build/pico2_w -DPICO_BOARD=pico2_w -DPICO_SDK_PATH=C:/path/to/pico-sdk
cmake --build build/pico2_w
```

生成された `yajir_pico2_w.uf2` をPico 2 Wへ書き込みます。USB serialはSDKのCDC stdioを使い、
実UART stdioは無効です。

## スクリプト投入

USB serial端末を開き、スクリプト本文を貼り付けた後、単独行で `@run` を送ります。

```text
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
@run
```

実行開始後のUSB入力は、1バイトずつ `ON USB_SERIAL` へpostされ、文字コードが `ARG[0]` に
入ります。ただし`Ctrl+C`（ASCII 0x03）はローダが予約しており、実行中のスクリプトを強制中断して
新しいスクリプトの受信待ちへ戻ります。受信途中やロードエラー後の`Ctrl+C`は入力内容を破棄します。

```text
^C
[loader] script stopped.
Paste a script, then enter @run on its own line.
>
```

これによりUSBを抜いたりボードをリセットしたりせず、投入、実行、中断、再投入を繰り返せます。
`Ctrl+C`は`ON USB_SERIAL`にはpostされません。ホスト側でブロッキングする`DELAY`の実行中は、
その処理が終わってUSB入力のポーリングへ戻った時点で中断されます。通常の`WAIT`中はすぐ反応します。

## 基本ポート

| ポート | 内容 |
|---|---|
| `LED1` | Pico 2 WオンボードLED。読み書き可能 |
| `STDOUT` | USB serialへUTF-8文字列または整数を出力 |
| `NOW` | 起動後のミリ秒 |
| `DELAY` | ブロッキング遅延 |
| `VMSIZE` | Pico設定でのVM arenaサイズ |
| `USB_SERIAL` | USB入力のイベント源。`ARG[0]`に1バイト |

## GPIO

Pico 2 Wの外部端子へ出ているGPIO 0..22、26..28を指定できます。不正な引数やピン番号では
産出値または`RESULT`が`-1`になります。

| ポート | 形式 | 内容 |
|---|---|---|
| `GPIO_GET` | `pin -> GPIO_GET` | 現在値0/1を産出 |
| `GPIO_SET` | `pin, value -> GPIO_SET` | 0/1を書き、書いた値を産出 |
| `GPIO_MODE` | `pin, mode -> GPIO_MODE` | 入出力とpullを設定 |
| `GPIO_TOGGLE` | `pin -> GPIO_TOGGLE` | 出力ラッチを反転し、反転後の値を産出 |

`GPIO_MODE`では以下の定数を指定します。出力値は`GPIO_SET`で設定してから使用します。

| 定数 | 値 | 内容 |
|---|---:|---|
| `GPIO_IN` | 0 | 入力、pullなし |
| `GPIO_OUT` | 1 | 出力 |
| `GPIO_IN_PULLUP` | 2 | pull-up入力 |
| `GPIO_IN_PULLDOWN` | 3 | pull-down入力 |

## GPIO IRQ

`pin, edge_mask -> GPIO_IRQ_ENABLE`で割り込みを開始し、`pin -> GPIO_IRQ_DISABLE`で停止します。
複数エッジは定数を加算して指定できます。

| 定数 | 値 | 内容 |
|---|---:|---|
| `GPIO_IRQ_RISE` | 1 | 立ち上がり |
| `GPIO_IRQ_FALL` | 2 | 立ち下がり |
| `GPIO_IRQ_HIGH` | 4 | High level |
| `GPIO_IRQ_LOW` | 8 | Low level |

`ON GPIO_IRQ`には`ARG[0]=pin`、`ARG[1]=edge_mask`、`ARG[2]=現在値`が渡されます。
レベル割り込みは条件が続く間にイベントが連続するため、通常は立ち上がり・立ち下がりを使用します。

## ADC

| ポート | 形式 | 内容 |
|---|---|---|
| `ADC_GET` | `channel -> ADC_GET` | ADC 0..2（GPIO 26..28）の12bit値0..4095 |
| `ADC_PIN` | `pin -> ADC_PIN` | GPIO 26..28を指定して12bit値を取得 |
| `ADC_TEMP` | `ADC_TEMP` | RP2350内部温度の概算値、摂氏 x100 |

`ADC_TEMP`はチップ内部温度センサーの公称変換式を使う概算値で、校正済みの周囲温度計ではありません。

## サンプル

- `scripts/pico2_w/blink.yaj`: オンボードLED点滅
- `scripts/pico2_w/gpio_chain.yaj`: スクリプト内ポートによる固定GPIOチェイン
- `scripts/pico2_w/gpio_irq.yaj`: GPIO 15のpull-up入力と両エッジ割り込み
- `scripts/pico2_w/adc_temp.yaj`: 内部温度を1秒ごとに表示
