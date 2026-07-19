# Yajir for Arduino Nano R4

Arduino Nano R4へYajirランタイムを一度書き込み、以後はUSB serialからYajirスクリプトを
投入して実行するホストです。Arduinoのスケッチをアプリケーションごとに書き換える必要はありません。

## 必要な環境

- Arduino CLI 1.5.1以降
- Arduino UNO R4 Boards 1.5.0以降
- FQBN `arduino:renesas_uno:nanor4`

```powershell
arduino-cli core update-index
arduino-cli core install arduino:renesas_uno
```

## ビルド

```bat
src\host\nano_r4\build.cmd
```

`arduino-cli`が`PATH`にない場合は、`ARDUINO_CLI`環境変数または
`-ArduinoCli C:\path\to\arduino-cli.exe`で場所を指定します。

成果物は`build/nano_r4`へ生成されます。Nano R4を接続して`-Upload`を指定すると、
COMポートを自動検出してビルドと書き込みを行います。

```bat
src\host\nano_r4\build.cmd -Upload
```

自動検出できない場合や複数台接続時は、COMポートを明示できます。

```bat
src\host\nano_r4\build.cmd -Upload -Port COM12
```

Nano R4が通常モードで認識されない場合は、RESETボタンを素早く2回押すとブートローダへ入ります。

## スクリプト投入

USB serialを115200bpsで開き、スクリプト全文を貼り付けたあと、独立した行で`@run`を送ります。
受信中は入力をエコーバックします。実行中にCtrl+Cを送るとスクリプトを停止し、受信待ちへ戻ります。

```text
==== Yajir on Arduino Nano R4 ====
Version: 0.4.9
VM arena: 5636 bytes
Source buffer: 4096 bytes
Paste a script, then enter @run or @save on its own line.
>
```

実行中の通常文字は`ON USB_SERIAL`へ1バイトずつpostされ、`ARG[0]`で受け取れます。

受信時には、各物理行の行頭スペース／タブと、行頭空白に続く`//`コメント専用行の本文を
ソースバッファへ保存しません。改行は残すため、コンパイルエラーの行番号は維持されます。
入力は圧縮前のままエコーバックされ、実行時には受信byte数、格納byte数、削減byte数を表示します。

文字列リテラルと文字リテラルの状態も追跡します。したがって、次のように`\`で継続された
文字列内の行頭`//`はコメントではなく文字列内容として保存されます。

```yajir
def_alias(TMP, "xxxxx\
    //xxxxx\
    //111111")
```

確認用の`scripts/nano_r4/source_compression.yaj`は、圧縮サイズを表示したあと
`xxxxx//xxxxx//111111`を出力します。

## データフラッシュautorun

スクリプト末尾の独立した行で`@save`を送ると、圧縮済みソースをコンパイルし、成功した場合だけ
RA4M1のデータフラッシュへ保存します。保存成功後は、その場でもスクリプトを実行します。

```text
MAIN
    1 -> LED1
    500 -> WAIT
    0 -> LED1
    500 -> WAIT
END
@save
```

```text
[loader] script received: ... bytes; stored: ... bytes; saved: ... bytes
[autorun] saving compiled script...
[autorun] saved: ... bytes
[loader] running. USB input is now sent to ON USB_SERIAL.
```

次回起動時は保存済みソースを`autorun.yaj`相当としてRAMへ読み、再コンパイルして自動実行します。
実行中にCtrl+Cを送れば停止して通常の受信待ちへ戻ります。保存内容はCtrl+Cでは消えません。

```text
[autorun] loaded: ... bytes
[autorun] running. Send Ctrl+C to stop it.
```

受信待ちで単独行の`@erase`を送ると保存済みautorunを削除します。`@run`は従来どおり、
保存内容を変更せずRAM上で一度だけ実行します。

Nano R4の8KBデータフラッシュのうち、先頭1KBを管理ヘッダ、続く4KBをソース領域として
このホストが予約します。残り3KBは使用しません。ヘッダには形式バージョン、長さ、CRC32を持ち、
本文の書込みと読戻し検証が成功した最後に有効マーカーを書きます。保存中に電源が切れた場合、
不完全な内容は次回起動時に無効として扱われます。その場合、以前のautorunも失われます。

データフラッシュには書換え寿命があるため、`@save`はスクリプト配備時に使用し、短周期で
繰り返し保存する用途には使わないでください。また、Arduinoの`EEPROM`ライブラリと先頭5KBを
共有できないため、このファームウェアとEEPROM利用コードを同時に組み合わせないでください。

確認用の`scripts/nano_r4/autorun_blink.yaj`には末尾の`@save`まで含まれています。

## 初期ポート

| ポート | 形式 | 内容 |
|---|---|---|
| `LED1` | `value -> LED1` / `LED1` | 単色の内蔵LEDを読み書き |
| `RGB_LED` | `red, green, blue -> RGB_LED` | RGB LEDを制御。各成分は`0=消灯`、非0＝点灯 |
| `RGB_PWM` | `red, green, blue -> RGB_PWM` | RGB LEDを各成分`0..65535`で調光 |
| `ADC_GET` | `channel -> ADC_GET` | ADCチャンネル0～7を14-bitで読み取る |
| `ADC_PIN` | `pin -> ADC_PIN` | A0～A7をピン番号で14-bit読み取り |
| `PWM_SET` | `pin, level -> PWM_SET` | PWMデューティーを`0..65535`で設定 |
| `PWM_GET` | `pin -> PWM_GET` | 最後に設定したPWMデューティーを取得 |
| `PWM_FREQ` | `pin, hz -> PWM_FREQ` | PWMピンが属するタイマーの周波数を設定 |
| `GPIO_GET` | `pin -> GPIO_GET` | 指定ピンを読み、`RESULT`へ0/1を返す |
| `GPIO_SET` | `pin, value -> GPIO_SET` | 指定ピンへ0/1を書き、値を返す |
| `GPIO_MODE` | `pin, mode -> GPIO_MODE` | 入出力モードを設定 |
| `GPIO_TOGGLE` | `pin -> GPIO_TOGGLE` | 出力を反転し、新しい値を返す |
| `GPIO_IRQ_ENABLE` | `pin, mask -> GPIO_IRQ_ENABLE` | GPIO IRQ条件を設定。`mask=0`で無効化 |
| `GPIO_IRQ_DISABLE` | `pin -> GPIO_IRQ_DISABLE` | 指定ピンのGPIO IRQを無効化 |
| `VMSIZE` | `VMSIZE` | Nano R4プロファイルのVMアリーナサイズ |
| `ON USB_SERIAL` | ハンドラ | USBから受け取った1バイトを`ARG[0]`へ渡す |
| `ON GPIO_IRQ` | ハンドラ | GPIO IRQのpin、edge、valueを受信 |

GPIO番号はD0-D13が`0..13`、A0-A7が`14..21`です。`GPIO_MODE`のモード名は
Pico 2 W版と共通で、`GPIO_IN`、`GPIO_OUT`、`GPIO_IN_PULLUP`、`GPIO_IN_PULLDOWN`です。
ただしArduinoCore-renesas 1.6.0はpull-down入力に未対応なので、Nano R4で
`GPIO_IN_PULLDOWN`を指定すると`-1`を返します。Ctrl+Cでは`GPIO_OUT`に設定したピンをLowへ戻します。

### ADC

`channel -> ADC_GET`はチャンネル0～7、`pin -> ADC_PIN`はA0～A7（`14..21`）を
Nano R4の14-bit ADCで読みます。`ADC_MAX`は最大値`16383`です。対象外では`-1`を返します。
Nano R4版には現在`ADC_TEMP`がなく、Pico 2 W固有の追加ポートとして扱います。

### PWM

PWM対応ピンはD3、D5、D6、D9、D10、D11です。デューティー範囲はPico 2 W版と同じ
`0..PWM_MAX`（`PWM_MAX=65535`）です。既定周波数も共通の`PWM_DEFAULT_FREQ=1000` Hzで、
`pin, hz -> PWM_FREQ`により変更できます。同じハードウェアタイマーを共有するピンでは
周波数も共有されます。対象外のピンまたは範囲外の値では`-1`を返します。
Ctrl+Cでは使用中のPWMを停止し、ピンをLowへ戻します。

内蔵RGB LEDを滑らかに調光するときは`RGB_PWM`を使います。各成分は`0`で消灯、
`PWM_MAX`（65535）で最大輝度です。内蔵RGB LEDもハードウェアPWMタイマーを使うため、
同じタイマーを共有する外部PWMピンの周波数変更は色の調光にも影響します。

### GPIO IRQ

`GPIO_IRQ_RISE=1`、`GPIO_IRQ_FALL=2`です。加算すると両エッジを検出します。
Nano R4では`GPIO_IRQ_LOW=8`も利用できます。HighレベルIRQはArduinoコアにないため、
`GPIO_IRQ_HIGH`はPico 2 W固有です。
割り込み機能を持たないピン、または同じIRQチャネルを使用中の別ピンを指定すると`-1`を返します。

```yajir
2, GPIO_IN_PULLUP -> GPIO_MODE
2, GPIO_IRQ_RISE + GPIO_IRQ_FALL -> GPIO_IRQ_ENABLE

ON GPIO_IRQ
	"pin=", ARG[0], " edge=", ARG[1], " value=", ARG[2] -> STDOUT
END
```

`ARG[0]`はピン番号、`ARG[1]`は発生したエッジ、`ARG[2]`は割り込み時の入力値です。
`pin -> GPIO_IRQ_DISABLE`またはCtrl+Cで無効化します。互換上、`mask=0`による無効化も受理します。
機械式スイッチではチャタリングにより
短時間に複数イベントが届くことがあります。

## 現在の小型プロファイル

- バイトコード: 1024 bytes
- 文字列プール: 256 bytes
- 文字列スロット: 32 bytes（本文は最大31 bytes）
- 圧縮ソース受信: 4096 bytes
- 識別子: 終端を含む17 bytes（本文は最大16 bytes）

この識別子制限により、`CFG_EVENT_QUEUE_LEN`など15文字を超える一部の`CFG_*`公開定数は
Nano R4プロファイルから直接参照できません。通常のポート名、別名、ハンドラ名には影響しません。

ArduinoCore-renesas 1.6.0での現在のビルド実測はFlash 80,884 bytes、静的RAM 15,224 bytesです。
32KB SRAMのうち17,544 bytesがスタックなどに残ります。8KBの無圧縮ソースバッファ版と比べ、
圧縮とデータフラッシュautorunの状態を含めても静的RAMを3,920 bytes削減しています。
