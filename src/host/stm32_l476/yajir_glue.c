/* yajir_glue.c - STM32 Nucleo-L476RG 用ホストグルー実装（ボード依存部）
 *
 * PCホスト(host_mock.c)の実機版。コンソール出力→USART2、GPIO擬似→実GPIO、に置き換えただけ。
 * 構造はPC版とそろえてあるので、見比べると「どこがボード依存か」が分かる。
 *
 * ※このファイルは STM32 HAL に依存するため、PC(MSVC)ではビルドできない。
 *   CubeIDE/CubeMX で生成したプロジェクトに本ファイル群と core/, host/common/ を追加してビルドする。
 */
#include <string.h>
#include "yajir_glue.h"
#include "script.h"
#include "host_diag.h"   /* host_diag_reset / host_diag_note（did-you-mean 候補収集） */

/* Nucleo-L476RG のボード割り当て
 * B1（PC13・青）は EXTI から yajir_post_button() でイベント源 BTN として発火する。
 * ポーリング読み用の in ポートは置かない（PCホストと顔ぶれを揃えるため, host/README.md）。 */
#define LED1_PORT     GPIOA
#define LED1_PIN      GPIO_PIN_5     /* LD2（緑） */

static UART_HandleTypeDef *s_uart = NULL;

/* ---- UART 出力（STDOUT／ローダ共用・ブロッキング送信） ---- */
void yajir_putc(char c)
{
    if (s_uart) HAL_UART_Transmit(s_uart, (uint8_t *)&c, 1, HAL_MAX_DELAY);
}
void yajir_puts(const char *s)
{
    if (s_uart && s) HAL_UART_Transmit(s_uart, (uint8_t *)s, (uint16_t)strlen(s), HAL_MAX_DELAY);
}

/* ---- 単調クロック（NOW） ---- */
int32_t get_tick(void) { return (int32_t)HAL_GetTick(); }

/* register + did-you-mean 候補収集をまとめる薄いラッパ（PC版と同じ流儀）。 */
static void reg_out  (const char *n, script_out_fn f){ script_register_out(n, f); host_diag_note(n); }
static void reg_inout(const char *n, script_in_fn g, script_out_fn s, script_type_t t){ script_register_inout(n, g, s, t); host_diag_note(n); }
static void reg_handler(const char *n){ script_register_handler(n); host_diag_note(n); }

/* STDOUT の整形（argc ループ/タグ判定/int→10進/改行）はコアの th_stdout_core が持つ（v0.4.8）。
 * ここは出力先＝シンク yajir_puts(const char*) を渡すだけ。手書き itoa は不要になった。 */

/* ---- LED1（PA5）：inout(int)。書き込み＋現在値を RESULT へ tee（§3, v0.3.8） ---- */
static int32_t led1_get(void)
{
    return (HAL_GPIO_ReadPin(LED1_PORT, LED1_PIN) == GPIO_PIN_SET) ? 1 : 0;
}
static void led1_set(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    HAL_GPIO_WritePin(LED1_PORT, LED1_PIN, v ? GPIO_PIN_SET : GPIO_PIN_RESET);
    script_set_result(led1_get());   /* 現在値を産出（int → RESULT） */
}

/* ---- DELAY：ブロッキング遅延（産出none の out, v0.3.8） ---- */
static void th_delay(int argc, const script_value_t *a)
{
    int32_t ms = (argc > 0) ? a[0].i : 0;
    if (ms > 0) HAL_Delay((uint32_t)ms);
}

/* ---- 束縛一覧（PC版 host_register_all と同じ顔ぶれの実機版） ---- */
void host_register_all(void)
{
    host_diag_reset();   /* 再ロード時も did-you-mean 候補を作り直す */

    /* PCホスト(host_mock.c)と同じ顔ぶれ。片方だけに足さないこと（host/README.md）。 */
    reg_inout("LED1",   led1_get, led1_set, SCRIPT_T_INT);  /* PA5 読み書き */
    reg_out  ("DELAY",  th_delay);                          /* HAL_Delay */

    /* コア昇格した2つ（v0.4.8）。NOW=SysTick ms（内部クロックの源）、STDOUT=USART2 へのシンク。 */
    script_register_now(get_tick);        /* HAL_GetTick */
    script_register_stdout(yajir_puts);   /* シグネチャ void(const char*) が一致 */

    /* イベント源（ハンドラ）。本体は ON BTN / ON UART1 としてスクリプトに書く。
     *   - BTN  : ボタンEXTI ISR が yajir_post_button() で発火（次tickで ON BTN）
     *   - UART1: 実行開始後の受信1文字を1イベントとして発火（ON UART1 で ARG[0]=文字） */
    reg_handler("BTN");
    reg_handler("UART1");

    /* ユーザ定義の名前付きハンドラ。ここでは登録するだけで、発火元はユーザが書く。
     * 自作の周辺（センサ完了ISR、DMA完了、受信パケット等）を ON MYHANDLER に繋ぐときの雛形:
     *     script_arg_t a[3];
     *     a[0] = SCRIPT_ARG_INT(x); a[1] = SCRIPT_ARG_INT(y); a[2] = SCRIPT_ARG_STR(s, strlen(s));
     *     script_post_msg_v("MYHANDLER", 3, a);   // ISR から呼んで可（キューはISR安全, v0.4.1）
     * PC版の host_fire_myhandler()（pc/host_mock.c）が動く実例。 */
    reg_handler("MYHANDLER");
}

void yajir_glue_init(UART_HandleTypeDef *huart)
{
    s_uart = huart;
}
