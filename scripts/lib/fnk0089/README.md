# FNK0089スクリプトライブラリ

必要なファイルをYAJIRドライブのルートへ置き、アプリの最上部から選択的に
`def_import`します。ライブラリ名は8.3形式に収まっています。

| ファイル | import | 内容 |
|---|---|---|
| `fnkmotor.yaj` | `def_import("fnkmotor")` | 4輪モーター、左右駆動 |
| `fnkbuzz.yaj` | `def_import("fnkbuzz")` | 連続音、時間指定ビープ |
| `fnkline.yaj` | `def_import("fnkline")` | 左・中央・右のライン検出 |
| `fnksonar.yaj` | `def_import("fnksonar")` | 超音波距離センサー |
| `fnkmat.yaj` | `def_import("fnkmat")` | 16x8ドットマトリックス |
| `fnkfont.yaj` | `def_import("fnkfont")` | 5x7英字フォント（A-Z、空白、!、?） |

importしたライブラリは自動でハードウェアを初期化しません。アプリの`INIT`から
各ライブラリの`_..._INIT`ポートを一度呼んでください。

複数ライブラリを使う場合も、必要なものだけをアプリの最上部へ並べます。

```yajir
def_import("fnkmat")
def_import("fnkfont")
```

## ポート一覧

### モーター

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `_MOTOR_INIT` | `0 -> _MOTOR_INIT` | `0` | 4輪を500 Hzで初期化して停止 |
| `_MOTOR1_`～`_MOTOR4_` | `speed -> _MOTOR1_` | 設定速度 | 各車輪を`-100..100`で駆動 |
| `_LEFT_` | `speed -> _LEFT_` | 設定速度 | 左前後輪を同時駆動 |
| `_RIGHT_` | `speed -> _RIGHT_` | 設定速度 | 右前後輪を同時駆動 |
| `_DRIVE_` | `left, right -> _DRIVE_` | `0` | 左右速度をまとめて設定 |

### ブザー

| ポート／ハンドラ | 形式 | 内容 |
|---|---|---|
| `_BUZZER_` | `hz -> _BUZZER_` | 指定周波数で連続発音。`0`で停止 |
| `_BEEP_` | `hz, ms -> _BEEP_` | 指定時間だけ非同期に発音 |
| `ON BUZZER_END` | 引数なし | `_BEEP_`の停止後に通知 |

ブザーはパッシブ型です。現在のライブラリはデューティ比50%固定で、音量を細かく
制御するポートは持ちません。

### ライントラッキング

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `_LINE_INIT` | `0 -> _LINE_INIT` | `0` | GPIO12/11/10を入力へ設定 |
| `LINE_L_` | `LINE_L_` | `0`または`1` | 左センサーの生入力 |
| `LINE_C_` | `LINE_C_` | `0`または`1` | 中央センサーの生入力 |
| `LINE_R_` | `LINE_R_` | `0`または`1` | 右センサーの生入力 |
| `LINE_BITS_` | `LINE_BITS_` | `0..7` | 左をbit2、中央をbit1、右をbit0に格納 |

### 超音波センサー

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `_SONAR_INIT` | `0 -> _SONAR_INIT` | `0` | GPIO4をTRIG、GPIO5をECHOへ設定 |
| `SONAR_US_` | `SONAR_US_` | echo幅（us） | 10 usトリガ後のHigh幅を取得 |
| `SONAR_CM_` | `SONAR_CM_` | 距離（整数cm） | echo幅を音速換算。timeout時`0` |

### ドットマトリックス

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `_MATRIX_INIT` | `0 -> _MATRIX_INIT` | `0` | I2C0、HT16K33、表示を初期化して消去 |
| `_MATRIX_COL_` | `x, pattern -> _MATRIX_COL_` | 下位8bit | 縦1列を表示バッファへ描画 |
| `_MATRIX_PIXEL_` | `x, y, value -> _MATRIX_PIXEL_` | `0`または`1` | 1 pixelを表示バッファへ描画 |
| `_MATRIX_CLEAR` | `0 -> _MATRIX_CLEAR` | `0` | バッファと実表示を消去 |
| `_MATRIX_SHOW` | `0 -> _MATRIX_SHOW` | `0` | バッファ全体を実表示へ転送 |
| `_MATRIX_BRIGHT` | `level -> _MATRIX_BRIGHT` | `0..15` | 表示輝度を設定 |

### 5x7フォント

| ポート | 形式 | 産出値 | 内容 |
|---|---|---:|---|
| `_FONT_COL_` | `ascii, column -> _FONT_COL_` | 縦8bitパターン | 文字の列`0..5`を取得 |

`column=0..4`が字形、`column=5`が文字間の空白です。収録文字は`A-Z`、空白、
`!`、`?`で、未収録文字は空白になります。

## モーター

```yajir
INIT
	0 -> _MOTOR_INIT
END

MAIN
	40, 40 -> _DRIVE_
END
```

速度は`-100..100`です。`_MOTOR1_`～`_MOTOR4_`、`_LEFT_`、`_RIGHT_`も
使用できます。車輪を浮かせ、すぐ電源を切れる状態で最初の確認をしてください。

## ブザー

```yajir
2000 -> _BUZZER_       // 連続発音
0 -> _BUZZER_          // 停止
2000, 120 -> _BEEP_    // 120msだけ発音

ON BUZZER_END
	"beep finished" -> STDOUT
END
```

ブザーのGPIO2とM1のGPIO18/19は同じPWM sliceを共有します。発音周波数の変更は
M1側PWM周波数にも影響するため、走行中の発音は当面避けてください。

## ライントラッキング

```yajir
INIT
	0 -> _LINE_INIT
END

MAIN
	LINE_L_, LINE_C_, LINE_R_, LINE_BITS_ -> STDOUT
	100 -> WAIT
END
```

`LINE_BITS_`は左をbit2、中央をbit1、右をbit0とした生入力値`0..7`です。

## 超音波センサー

```yajir
INIT
	0 -> _SONAR_INIT
END

MAIN
	SONAR_CM_ -> STDOUT
	100 -> WAIT
END
```

`SONAR_US_`はecho幅、`SONAR_CM_`は整数cmを返します。応答がない場合は`0`です。
一回の計測は同期処理で、最大約18msブロックします。

## ドットマトリックス

実機は8x8ユニットを左右へ2枚並べた、横16×縦8 pixelです。ライブラリの論理座標は
左上を`(0, 0)`とし、xは左から右へ`0..15`、yは上から下へ`0..7`です。

```yajir
INIT
	0 -> _MATRIX_INIT
	0, 0x3C -> _MATRIX_COL_
	1, 0x42 -> _MATRIX_COL_
	0 -> _MATRIX_SHOW
END
```

`_MATRIX_COL_`は物理表示の左から右へx=`0..15`、上から下へ下位bitから並ぶ
縦8bitパターンを表示バッファへ描き、
`_MATRIX_SHOW`で実機へまとめて転送します。`_MATRIX_PIXEL_`はx、y、値の順です。
明るさは`0..15 -> _MATRIX_BRIGHT`で設定します。ライブラリは表示バッファとして
`GVAR[24]..GVAR[31]`を占有します。

ドットマトリックスと超音波センサーは、どちらも基板上のGPIO4/5コネクタを使います。
同時には装着・使用せず、どちらか一方のライブラリだけを初期化してください。

### 英字フォント

`fnkfont.yaj`はASCIIコードと列番号`0..5`から、縦8bitの1列を返します。
列`0..4`が5x7字形、列`5`が文字間の空白です。未収録文字は空白になります。

```yajir
"A" -> ASC -> VAR[0]
VAR[0], 0 -> _FONT_COL_ -> STDOUT
```

`matrix_ticker.yaj`は`fnkmat`と`fnkfont`を組み合わせた任意文字列のスクロール例です。
`TICKER_TEXT`を書き換えるだけで、収録文字を自由に流せます。

フォント表は読みやすさを優先して10進数のCSV文字列として保持しています。Yajirの
`STRTOL`は基数省略時に16進として扱うため、ライブラリ内では必ず次のように10進を指定します。

```yajir
FNK_FONT_DATA, ",", INDEX -> FIELD
SRESULT, 10 -> STRTOL
```

## 同時動作

ドットマトリックスはI2C、ブザーはPWM、ティッカーの時間進行は`MAIN`、時間指定ビープは
`AFTER`と`ON BUZZER_END`を使うため、表示と演奏は一つのスクリプト内で並行して進められます。
ただしブザーとM1はPWM sliceを共有するため、演奏中はM1を停止してください。
