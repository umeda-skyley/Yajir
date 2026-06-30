/* yajir_loader.c - yajir_loader.h の実装（STM32 Nucleo-L476RG）
 *
 * 受信→@run検出→script_load→script_tick の常駐ループ。エラー表示は common/host_diag。
 * ※STM32 HAL 非依存（UART送受信は yajir_glue 経由）。固定RAMのみ・ヒープ不使用。
 */
#include <string.h>
#include "yajir_loader.h"
#include "yajir_glue.h"
#include "script.h"
#include "vm.h"          /* 静的アリーナのサイズ算出のためだけに参照 */
#include "host_diag.h"   /* script_strerror / host_suggest_name */

/* ---- 固定アリーナ（ヒープ不使用。ホストが用意する固定RAM領域, §12） ---- */
static char g_arena[sizeof(script_vm_t) + 128];

/* ---- スクリプト受信バッファ ---- */
#define SCRIPT_MAX  4096
static char  g_src[SCRIPT_MAX];
static int   g_len;        /* 受信済みバイト数 */
static int   g_running;    /* 0=受信中 / 1=実行中 */
static volatile int g_btn_pending;  /* ISR→ループへ橋渡し（ボタン押下フラグ） */

/* ---- 小物：10進プリント ---- */
static void put_dec(int32_t v)
{
    char b[12]; int i = 0; uint32_t u = (v < 0) ? (yajir_putc('-'), (uint32_t)(-v)) : (uint32_t)v;
    do { b[i++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    while (i) yajir_putc(b[--i]);
}

static void banner(void)
{
    yajir_puts("\r\n==== Yajir on Nucleo-L476RG ====\r\n");
    yajir_puts("paste your script, then a line:  @run\r\n");
    yajir_puts("(reset the board to load again)\r\n> ");
}

void yajir_loader_init(void)
{
    g_len = 0;
    g_running = 0;
    g_btn_pending = 0;
    memset(g_src, 0, sizeof(g_src));
    banner();
}

/* @run/@fin を検出してロード＆実行する。戻り値: 1=ロード起動した / 0=継続受信 */
static int try_trigger(void)
{
    int run;
    if (g_len < 4) return 0;
    /* バッファ末尾が "@run"/"@fin" か？ */
    if      (memcmp(g_src + g_len - 4, "@run", 4) == 0) run = 1;
    else if (memcmp(g_src + g_len - 4, "@fin", 4) == 0) run = 0;
    else return 0;

    g_len -= 4;              /* センチネルを巻き戻して本文だけ残す */
    g_src[g_len] = '\0';
    yajir_puts("\r\n[loader] script received (");
    put_dec(g_len);
    yajir_puts(" bytes)\r\n");

    if (!run) { yajir_puts("[loader] @fin: stored, not running.\r\n"); return 1; }

    /* init → register（def_*の実体）→ load（受信→コンパイル→常駐, §11） */
    script_init(g_arena, sizeof(g_arena));
    host_register_all();

    if (script_load(g_src, (size_t)g_len) != 0) {
        const script_error_t *e = script_last_error();
        yajir_puts("[loader] load error: line ");
        put_dec(e->line);
        yajir_puts(": ");
        yajir_puts(script_strerror(e->code));
        if (e->tok[0]) { yajir_puts(" '"); yajir_puts(e->tok); yajir_puts("'"); }
        if ((e->code == ERR_UNKNOWN_PORT || e->code == ERR_UNKNOWN_NAME) && e->tok[0]) {
            const char *sug = host_suggest_name(e->tok);
            if (sug) { yajir_puts(" (did you mean '"); yajir_puts(sug); yajir_puts("'?)"); }
        }
        yajir_puts("\r\n");
        return 1;   /* 失敗：実行はしない。やり直しはリセット */
    }

    yajir_puts("[loader] running.\r\n");
    g_running = 1;
    return 1;
}

void yajir_feed_byte(uint8_t b)
{
    if (g_running) {
        /* 実行中：受信文字を UART1 イベントへ（ON UART1 で ARG[0]=文字 として受ける） */
        script_post_msg_char("UART1", (char)b);
        return;
    }
    if (g_len < SCRIPT_MAX - 1) {
        g_src[g_len++] = (char)b;
        try_trigger();
    }
    /* バッファ満杯は黙って捨てる（実機では稀。必要なら警告を出してもよい） */
}

void yajir_post_button(void)
{
    g_btn_pending = 1;   /* ISR内ではフラグだけ立て、post はループ側で（軽いISRに） */
}

void yajir_loop(void)
{
    if (!g_running) return;

    if (g_btn_pending) {
        g_btn_pending = 0;
        script_post_msg("BTN", 0);   /* 次tickで ON BTN が走る */
    }
    script_tick();   /* タイマ満期＋キュー＋周期＋MAIN を1ステップ（§1） */
}
