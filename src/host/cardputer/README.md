# Yajir for M5Stack Cardputer

M5Stack CardputerへYajir coreを載せ、USB serialからYajirスクリプトを投入して実行する
ホストです。現在の第1段階では、coreのコンパイル、TFTとUSBへの出力、USBローダーを実装しています。

## 必要な環境

- Arduino CLI 1.5.1以降
- M5Stack Boards 3.2.2以降
- M5Cardputer 1.1.0以降
- M5Unified 0.2.8以降
- M5GFX 0.2.10以降
- FQBN `m5stack:esp32:m5stack_cardputer`

## ビルド

```bat
src\host\cardputer\build.cmd
```

`arduino-cli`がPATHにない場合は、`ARDUINO_CLI`環境変数または`-ArduinoCli`引数で指定します。
成果物は`build/cardputer`へ生成されます。実機接続後は、検出されたCOMポートへ書き込めます。
ESP32ツールチェインの日本語パス問題を避けるため、中間生成物だけWindowsの一時ディレクトリに
作成し、書き込み用バイナリを`build/cardputer`へ出力します。

```bat
src\host\cardputer\build.cmd -Upload
```

## 現在の機能

- Yajir core v0.4.12を無変更でビルド
- USB serialを115200bpsで使用
- USBからスクリプトを受信し、独立した行の`@run`でコンパイル・実行
- USB／内蔵キーボード入力時にコメント専用行と行頭空白を圧縮してRAMへ格納
- スクリプト受信中のエコーはUSBだけへ返し、高速ペースト時の取りこぼしを抑制
- ローダー待機中は内蔵キーボードからも入力でき、`@run`と`Ctrl+C`を操作可能
- 実行中の`Ctrl+C`で停止し、スクリプト受信待ちへ復帰
- `STDOUT`をUSB serialとCardputer TFTへ同時出力
- 単一`DISPLAY`ポートとYajirライブラリによる文字・図形・SD画像描画
- `NOW`
- `ON USB_SERIAL`へのUSB受信文字イベント
- `ON KEYBOARD`へのCardputer内蔵キーボード押下イベント
- 単一`SPEAKER`ポートとYajirライブラリによる内蔵スピーカー制御
- microSD上の`/autorun.yaj`自動実行と`def_import`

```yaj
MAIN
    "Hello from Yajir!" -> STDOUT
END
@run
```

## 圧縮ロード

USB serialまたは内蔵キーボードから入力したスクリプトは、受信と同時に次の要素を除いて
ソースバッファへ格納します。

- 行頭のスペースとタブ
- 行頭空白に続く`//`コメント専用行の本文

改行は残すため、コンパイルエラーの行番号は原文と一致します。文字列と文字リテラルの状態を
追跡しているため、`\`で継続した文字列の次行が`//`で始まっていてもコメントとは扱いません。
行末コメントはそのまま保存します。圧縮効果を高めたい場合は、コメントを命令の後ろではなく
専用行へ書きます。

実行時には原文の受信量、格納量、削減量を表示します。

```text
[loader] script received: 5508 bytes; stored: 3077 bytes; saved: 2431 bytes
```

microSDから直接読み込む`/autorun.yaj`と`@load`対象ファイル、および`def_import`ライブラリは
圧縮せず、そのままコンパイラへ渡します。

## 内蔵キーボード

新しく押された文字キーごとに`KEYBOARD`イベントをpostします。キーを押し続けても自動リピート
しません。同時押しでは、新たに加わったキーだけがイベントになります。

ローダー待機中のキー入力はスクリプト受信バッファへ送られ、TFTへローカルエコーされます。
`@run`とEnterで実行し、`Ctrl+C`で入力消去または実行中スクリプトの停止ができます。
実行中の通常キーは従来どおり`ON KEYBOARD`へ送られます。

SD直下のスクリプトは、ローダープロンプトから名前を指定して実行できます。拡張子を
省略すると`.yaj`を補います。ファイル名は8文字以内の英数字、`_`、`-`に限定されます。

```text
@load "piano"
@load "piano.yaj"
```

`KEYBOARD`と`USB_SERIAL`は同じ引数位置を使用します。

| 値 | `KEYBOARD` | `USB_SERIAL` |
|---|---|---|
| `ARG[0]` | 文字コード | 受信byte |
| `ARG[1]` | 修飾キービットの論理和 | `0` |
| `SARG[2]` | 1文字 | 受信した1文字 |

Enter、Backspace、Tabの文字コードはそれぞれ`13`、`8`、`9`です。

修飾キーには`KEY_MOD_SHIFT`、`KEY_MOD_CTRL`、`KEY_MOD_ALT`、`KEY_MOD_FN`、
`KEY_MOD_OPT`を使用します。特殊キーの比較用に`KEY_ENTER`、`KEY_BACKSPACE`、`KEY_TAB`も
登録されています。修飾キーだけを押した場合はイベントを発生させません。

```yaj
ON KEYBOARD
    "key='", SARG[2], "' code=", ARG[0], " mods=", ARG[1] -> STDOUT
END

MAIN
END
```

## TFT文字表示

組込みポートは可変引数の`DISPLAY`ひとつです。第1引数にM5GFX操作名を指定し、
成功時は`0`、未知の操作は`-1`、引数不正は`-2`、SDまたは画像エラーは`-3`を返します。
幅、高さ、文字倍率などの照会操作は、その値を直接返します。

通常のアプリでは`scripts/lib/cardputer/display.yaj`をSDの`/lib/display.yaj`へ置き、
先頭でインポートします。ライブラリが`DISPLAY`呼び出しを隠し、用途別のスクリプト内ポートを
提供します。

```yaj
def_import("display")

2 -> TFT_TEXT_SIZE
TFT_YELLOW, TFT_BLACK -> TFT_TEXT_COLOR
20, 40 -> TFT_CURSOR
"large text" -> TFT_TEXT
```

| ポート | 送信値 | 内容 |
|---|---|---|
| `TFT_TEXT_SIZE` | `1..8` | 標準フォントの表示倍率を設定 |
| `TFT_TEXT_SIZE_` | なし | 現在の表示倍率を返す |
| `TFT_TEXT_COLOR` | `fg, bg` | 前景色と背景色を設定 |
| `TFT_CURSOR` | `x, y` | 次に描画する文字の左上座標 |
| `TFT_TEXT` | `text` | TFTだけへ文字列を描画。改行しない |
| `TFT_CLEAR` | `color` | 指定色で全画面消去し、カーソルを原点へ戻す |
| `TFT_PIXEL` | `x, y, color` | 1ピクセル描画 |
| `TFT_LINE` | `x0, y0, x1, y1, color` | 線を描画 |
| `TFT_RECT` | `x, y, w, h, color` | 矩形の枠を描画 |
| `TFT_FILL_RECT` | `x, y, w, h, color` | 塗りつぶし矩形 |
| `TFT_CIRCLE` | `x, y, r, color` | 円の枠を描画 |
| `TFT_FILL_CIRCLE` | `x, y, r, color` | 塗りつぶし円 |
| `TFT_BRIGHTNESS` | `0..255` | バックライト輝度 |
| `TFT_IMAGE` | `path, x, y, width, height` | SD上のJPEG、PNG、BMP、QOIを描画 |
| `TFT_FRAME_BEGIN` | `background_color` | 裏画面を指定色で消去し、描画先にする |
| `TFT_FRAME_END` | `0` | 裏画面をTFTへ一括転送する |

基本色として`TFT_BLACK`、`TFT_WHITE`、`TFT_RED`、`TFT_GREEN`、`TFT_BLUE`、
`TFT_CYAN`、`TFT_MAGENTA`、`TFT_YELLOW`を使用できます。画面サイズは
`TFT_WIDTH`と`TFT_HEIGHT`です。画像の`width`と`height`に`0`を渡すと元サイズを使います。
片方だけを`0`にすると、指定した辺に合わせて縦横比を維持します。

低レベルの`DISPLAY`は、`setTextSize`、`getTextSize`、`setTextColor`、`setCursor`、
`print`、`fillScreen`、`drawPixel`、`drawLine`、`drawRect`、`fillRect`、
`drawCircle`、`fillCircle`、`setBrightness`、`width`、`height`、`image`を受け付けます。
新しい用途別ポートは、C側の登録ポートを増やさず`display.yaj`へ追加できます。

確認用スクリプトは`scripts/cardputer/text_size.yaj`と
`scripts/cardputer/ticker.yaj`です。

### フレーム描画

画像や図形を動かす場合は、画面を直接消去して描き直すと途中経過が見えてちらつきます。
`TFT_FRAME_BEGIN`から`TFT_FRAME_END`までの描画はRGB565の裏画面へ送られ、最後に
TFTへ一括転送されます。既存の文字、図形、画像ポートをそのまま使用できます。

```yaj
TFT_BLACK -> TFT_FRAME_BEGIN
CAT, 4, 0 -> _IMG_MOVE_ -> _IMG_DRAW_ -> CAT
0 -> TFT_FRAME_END
```

裏画面は初回使用時に約64.8KBを動的確保し、スクリプトを`Ctrl+C`で停止したときに解放します。
確保に失敗した場合、`TFT_FRAME_BEGIN`は`-5`を返します。フレームを開始していない状態の
`TFT_FRAME_END`は`-3`です。フレーム描画を使わない既存スクリプトの動作は変わりません。

### 画像オブジェクト

`scripts/lib/cardputer/image.yaj`をインポートすると、SD上の画像を位置と表示サイズを持つ
画像オブジェクトとして扱えます。画像本体はRAMへ常駐せず、描画時にSDから読み出します。
管理できる画像は8個です。

```yaj
def_import("display")
def_import("image")

"img/cat.png" -> _IMG_LOADER_ -> SGVAR[0]
SGVAR[0], 32, 32 -> _IMG_RESIZE_ -> SGVAR[0]
SGVAR[0], 20, 50 -> _IMG_LOCATE_ -> _IMG_DRAW_ -> SGVAR[0]
```

| ポート | 形式 | 内容 |
|---|---|---|
| `_IMG_LOADER_` | `path -> _IMG_LOADER_` | SD上の画像を登録し、画像値を返す |
| `_IMG_MOVE_` | `image, dx, dy -> _IMG_MOVE_` | 現在位置から相対移動 |
| `_IMG_LOCATE_` | `image, x, y -> _IMG_LOCATE_` | 絶対座標へ移動 |
| `_IMG_RESIZE_` | `image, width, height -> _IMG_RESIZE_` | 描画サイズを変更 |
| `_IMG_ADD_` | `image1, image2 -> _IMG_ADD_` | 画像または描画リストを後ろへ追加 |
| `_IMG_DRAW_` | `image -> _IMG_DRAW_` | 画像またはリストを順番に描画し、同じ値を返す |
| `_IMG_RELEASE_` | `image -> _IMG_RELEASE_` | 使用している画像スロットを解放 |

画像値の実体は`id:path:x:y:width:height`形式の文字列です。`STDOUT`で確認できますが、
通常のアプリでは内部形式を分解せず、各画像ポートへ渡してください。`_IMG_ADD_`の結果へ
さらに画像を追加でき、最大8画像を登録順に重ねて描画します。描画リストを含む文字列は
255 bytesまでです。

ファイル名には`,`、`:`、`\`、`..`を使用できません。対応形式はJPEG、PNG、BMP、QOIです。
登録失敗時のIDは負数で、`-2`はパスまたは引数不正、`-3`はSD・ファイル・画像形式のエラー、
`-4`は画像スロット不足です。サンプル`scripts/cardputer/imgdemo.yaj`は、
microSDの`/img/cat.png`を32x32へ縮小して画面上で移動します。

低レベルの`DISPLAY`には、画像管理用として`loadImage`、`imageWidth`、`imageHeight`、
`drawImage`、`releaseImage`、`releaseImages`、`imageSlots`もあります。フレーム描画用として
`beginFrame`、`endFrame`、`frameActive`を受け付けます。

## microSD

FAT16またはFAT32でフォーマットしたmicroSDを使用します。起動時にカードをマウントし、
SD直下の`/autorun.yaj`があれば自動的にロード・コンパイル・実行します。カードがない、
マウントできない、ファイルがない、コンパイルに失敗した場合はUSBローダーへ戻ります。

`INIT`中の即時イベント送信はコアの仕様により破棄されます。インポートしたハンドラを
起動直後に呼ぶ場合は、RUN開始後に実行されるよう`AFTER 0`を付けます。

```yaj
INIT
    none -> SD_HELLO AFTER 0
END
```

```text
/
|-- autorun.yaj
`-- lib/
    `-- hello.yaj
```

`def_import("hello")`は最初に`/lib/hello.yaj`、次に`/hello.yaj`を探索します。ライブラリ名は
8文字以内の英数字、`_`、`-`に限定されます。importファイルは8191 bytesまで、
`autorun.yaj`およびUSBローダーへ投入するスクリプトは32767 bytesまでです。
1スクリプトから最大12ライブラリをimportできます。コンパイル後の上限は、
バイトコード16384 bytes、文字列定数プール8192 bytesです。

| ポート | 種別 | 内容 |
|---|---|---|
| `SD_READY` | in | マウント済みなら`1`、それ以外は`0` |
| `SD_SIZE` | in | カード容量をMiB単位で返す |

リポジトリ上では、再利用ライブラリを`scripts/lib/cardputer/`、`MAIN`を持つ実行サンプルを
`scripts/cardputer/`に配置します。microSDへコピーするときは、ライブラリを`/lib/`へ、
実行するサンプルを`/autorun.yaj`として配置します。

## 内蔵スピーカー

組込みポートは可変引数の`SPEAKER`ひとつです。`tone`、`stop`、`setVolume`、
`getVolume`、`isPlaying`を受け付けます。成功時は設定値または状態、未知の操作は`-1`、
引数不正は`-2`を返します。
`tone`は単音用の固定チャンネルを使用し、新しい音は再生中の音を置き換えます。
`Ctrl+C`でスクリプトを停止した場合は、再生中の音も強制停止します。

```yaj
"setVolume", 128 -> SPEAKER
"tone", 440, 300 -> SPEAKER
"isPlaying" -> SPEAKER -> STDOUT
```

通常は`scripts/lib/cardputer/sound.yaj`をインポートします。従来の`SPEAKER_TONE`、
`SPEAKER_VOLUME`、`SPEAKER_STOP`に加え、音階定数、`_BUZZER_`、`_BEEP_`を
スクリプト内ポートとして提供するため、アプリ側IFは変わりません。
`scripts/cardputer/piano.yaj`は、内蔵キーボードを1オクターブの鍵盤として使用するサンプルです。

ライブラリの公開IFはFNK0089版と共通です。

| ポート／ハンドラ | 形式 | 内容 |
|---|---|---|
| `_BUZZER_` | `hz -> _BUZZER_` | 指定周波数で連続発音。`0`で停止 |
| `_BEEP_` | `hz, ms -> _BEEP_` | 指定時間だけ非同期に発音 |
| `ON BUZZER_END` | 引数なし | `_BEEP_`の発音終了後に通知 |

Cardputerでは、これに加えて組込みポート`SPEAKER_VOLUME`で音量を調整できます。

## 次の段階

バッテリー、赤外線を順次追加します。
