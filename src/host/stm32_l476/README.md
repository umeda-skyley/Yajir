# Yajir host for STM32 Nucleo-L476RG

Tera Term でスクリプトを貼り付けて実行する、最小構成の実機ホストサンプルです。
**Lチカ**と**ボタン押下検出**が動くところまでをカバーします。

> このディレクトリの `.c/.h` は STM32 HAL に依存するため、PC(MSVC)ではビルドできません。
> CubeIDE / CubeMX で作ったプロジェクトに組み込んでビルドしてください（手順は下記）。

---

## ボード割り当て（Nucleo-L476RG 既定）

| 機能 | ピン | 備考 |
|---|---|---|
| LED1（ポート `LED1`） | **PA5** | ユーザLED LD2（緑） |
| ボタン（イベント `BTN` / ポート `SW1`） | **PC13** | ユーザボタン B1（青・アクティブLow, EXTI13） |
| UART（`STDOUT` と スクリプト受信） | **USART2** | ST-LINK 仮想COMポート経由でPCへ。**115200 8N1** |

USART2 は Nucleo の ST-LINK に配線済みなので、USBケーブル1本でPCと繋がります（追加配線不要）。

---

## CubeMX 設定（新規プロジェクトを作る場合）

1. ボード **NUCLEO-L476RG** を選択（LD2/B1/USART2 が自動設定される）。
2. **USART2**: Asynchronous, 115200 / 8 / None / 1。NVIC で **USART2 global interrupt** を有効化。
3. **PC13**: GPIO_EXTI13, Trigger = Falling edge。NVIC で **EXTI line[15:10] interrupt** を有効化。
4. **PA5**: GPIO_Output（LD2 のままでOK）。
5. SysTick はデフォルトのまま（`HAL_GetTick()` を NOW に使用）。

CubeMX 既定のシンボル名（`huart2` 等）を前提にしています。

---

## プロジェクトへの組み込み

ソースツリーから次を CubeIDE プロジェクトに追加します（コアは無改造で共有）:

```
src/core/*.c            … 言語コア（プラットフォーム非依存）
src/host/common/*.c     … host_diag.c（エラー文字列＋did-you-mean・共通）
src/host/stm32_l476/*.c … yajir_glue.c / yajir_loader.c（このボード固有）
```

インクルードパス（CubeIDE の Project > Properties > C/C++ Build > Settings > Include paths）に
`src/core` `src/host/common` `src/host/stm32_l476` を追加します。

> `script_config.h` の `CFG_MAX_PORTS` は 48 以上であること（組込み21＋ホスト分）。
> 既定で 48 になっています。超過すると register が**黙って失敗**する点に注意。

---

## main.c への結線（CubeMX 生成コードに数行足すだけ）

```c
/* USER CODE BEGIN Includes */
#include "yajir_glue.h"
#include "yajir_loader.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
static uint8_t g_rx;   /* UART 1バイト受信用 */
/* USER CODE END PV */

int main(void)
{
    /* ... HAL_Init / SystemClock_Config / MX_*_Init() ... */

    /* USER CODE BEGIN 2 */
    yajir_glue_init(&huart2);            /* STDOUT/ローダ出力に USART2 を使う */
    yajir_loader_init();                 /* バナー表示＋受信バッファ初期化 */
    HAL_UART_Receive_IT(&huart2, &g_rx, 1);   /* 1バイト受信割り込みを起動 */
    /* USER CODE END 2 */

    while (1)
    {
        /* USER CODE BEGIN 3 */
        yajir_loop();                    /* 実行中なら script_tick() を1回 */
        /* USER CODE END 3 */
    }
}

/* USER CODE BEGIN 4 */
/* UART受信完了：1バイトをローダへ渡し、次の1バイト受信を再アーム */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        yajir_feed_byte(g_rx);
        HAL_UART_Receive_IT(huart, &g_rx, 1);
    }
}

/* ボタンEXTI：押下を BTN イベントへ（次tickで ON BTN） */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_13) {
        yajir_post_button();
    }
}
/* USER CODE END 4 */
```

これだけです。`yajir_glue.c` / `yajir_loader.c` を追加し、上の数行を足すだけで動きます。

---

## 使い方（Tera Term）

1. ビルド＆書き込み後、Tera Term で ST-Link の COM ポートへ接続（**115200 8N1**）。
2. リセットするとバナーが出ます:
   ```
   ==== Yajir on Nucleo-L476RG ====
   paste your script, then a line:  @run
   (reset the board to load again)
   >
   ```
3. スクリプト（例: `scripts/blink.txt`）を**そのまま貼り付け**ます。
4. 最後に **`@run`** と打って Enter。これで コンパイル＆実行が始まります。
   ```
   [loader] script received (NN bytes)
   [loader] running.
   ```
5. やり直したいときは**ボードをリセット**して再度貼り付け。

> 改行は CR でも CRLF でもOK（`CFG_LINE_DELIM` 既定）。Tera Term の改行設定はそのままで大丈夫です。
> ロードせず保存だけしたいときは `@run` の代わりに `@fin`。

### サンプルスクリプト

| ファイル | 内容 |
|---|---|
| [`scripts/blink.txt`](../../../scripts/blink.txt) | 500ms 点滅（LD2） |
| [`scripts/button.txt`](../../../scripts/button.txt) | B1 を押すたびに LED トグル＋メッセージ |

`button.txt` を貼って `@run` したあと B1 を押すと、押すたびに `LED ON` / `LED OFF` が
ターミナルに出て LD2 が切り替わります。

---

## このサンプルが公開するポート

| 名前 | 種別 | 説明 |
|---|---|---|
| `LED1` | inout(int) | PA5。`1 -> LED1` で点灯、`LED1` 読みで現在値 |
| `SW1` | in(int) | PC13 をポーリング読み（押下で1）。ポーリングしたいとき用 |
| `NOW` | in(int) | 起動からの経過 ms（`HAL_GetTick`） |
| `STDOUT` | out | USART2 へ出力 |
| `DELAY` | out | `ms -> DELAY` でブロッキング遅延（`HAL_Delay`） |
| `BTN` | handler | ボタン押下イベント（`ON BTN … END`） |
| `UART1` | handler | 実行開始後の受信1文字（`ON UART1` で `ARG[0]`=文字） |

組込みポート（`STDOUT` 以外の文字列ユーティリティ等）は core が提供します。
一覧は [`docs/builtin_ports.md`](../../../docs/builtin_ports.md) を参照。

---

## 仕組み（PC版との対応）

- このホストは PC版（`src/host/pc/`）の実機置き換えです。構造をそろえてあるので見比べられます。
  - `th_stdout`: コンソール → **USART2 送信**
  - `LED1`: 擬似GPIO → **実GPIO(PA5)**
  - 擬似割り込み（キーボード）→ **UART RX 割り込み / ボタンEXTI**
- 「ISR安全が要るのは `post_*`（キュー積み）だけ」という言語仕様（§10）通り、
  ISR からは `script_post_msg*` しか呼んでいません。コンパイルや tick はメインループ側です。
