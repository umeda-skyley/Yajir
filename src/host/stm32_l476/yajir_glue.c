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

/* Nucleo-L476RG のボード割り当て */
#define LED1_PORT     GPIOA
#define LED1_PIN      GPIO_PIN_5     /* LD2（緑） */
#define BUTTON_PORT   GPIOC
#define BUTTON_PIN    GPIO_PIN_13    /* B1（青・アクティブLow） */

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
static void reg_in   (const char *n, script_in_fn  f, script_type_t t){ script_register_in (n, f, t); host_diag_note(n); }
static void reg_inout(const char *n, script_in_fn g, script_out_fn s, script_type_t t){ script_register_inout(n, g, s, t); host_diag_note(n); }
static void reg_handler(const char *n){ script_register_handler(n); host_diag_note(n); }

/* ---- STDOUT：値を USART2 へ。char型タグ=文字 / str=文字列 / int=10進数 ---- */
static void int_to_str(int32_t v, char *buf)   /* 簡易 itoa（snprintf不使用でも可） */
{
    char tmp[12]; int i = 0, neg = 0; uint32_t u;
    if (v < 0) { neg = 1; u = (uint32_t)(-(v + 1)) + 1u; } else u = (uint32_t)v;
    do { tmp[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (neg) tmp[i++] = '-';
    { int j = 0; while (i) buf[j++] = tmp[--i]; buf[j] = '\0'; }
}
static void th_stdout(int argc, const script_value_t *a)
{
    int i; char num[16];
    for (i = 0; i < argc; i++) {
        if (a[i].tag == SV_CHAR)          yajir_putc((char)a[i].i);                 /* char型タグ→文字 */
        else if (script_val_is_str(a[i])) yajir_puts(script_resolve_str(a[i]));     /* 文字列定数/スロット */
        else { int_to_str(a[i].i, num);   yajir_puts(num); }                        /* int型タグ→数値 */
    }
    yajir_puts("\r\n");
}

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

/* ---- BUTTON（PC13）：入力ポートとしての読み取りも提供（アクティブLow → 押下で1） ---- */
static int32_t button_get(void)
{
    return (HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == GPIO_PIN_RESET) ? 1 : 0;
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

    reg_inout("LED1",   led1_get,   led1_set, SCRIPT_T_INT);  /* PA5 読み書き */
    reg_in   ("SW1",    button_get,           SCRIPT_T_INT);  /* PC13 をポーリング読みする入力 */
    reg_in   ("NOW",    get_tick,             SCRIPT_T_INT);  /* SysTick ms */
    reg_out  ("STDOUT", th_stdout);                           /* USART2 出力 */
    reg_out  ("DELAY",  th_delay);                            /* HAL_Delay */

    /* イベント源（ハンドラ）。本体は ON BTN / ON UART1 としてスクリプトに書く。
     *   - BTN  : ボタンEXTI ISR が yajir_post_button() で発火（次tickで ON BTN）
     *   - UART1: 実行開始後の受信1文字を1イベントとして発火（ON UART1 で ARG[0]=文字） */
    reg_handler("BTN");
    reg_handler("UART1");
}

void yajir_glue_init(UART_HandleTypeDef *huart)
{
    s_uart = huart;
}
