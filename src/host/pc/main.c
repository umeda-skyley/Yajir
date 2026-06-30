/* main.c - PCホストのエントリ＋擬似割り込みドライバ（仕様 §1 メインループ）
 *
 *   script run <file> [seconds]
 *
 * 実機の `while(1){ if(uart_block_ready()) script_load(...); script_tick(); }` を
 * PCで再現する。スクリプトはUART流し込みの代替としてテキストファイルから読む。
 * （実機ローダの例は src/host/stm32_l476/yajir_loader.c を参照）
 *
 * 擬似割り込みドライバ（PC専用）:
 *   - キーボード入力を UART1 受信イベントとして post（印字キー＝1文字エコー）
 *   - TAB キー / 3秒ごとの自動発火 を BTN 押下イベントとして post
 *   実機ではこれらは UART RX ISR / ボタンEXTI ISR から script_post_msg* を呼ぶ箇所。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <conio.h>
#include "script.h"
#include "vm.h"          /* 静的アリーナのサイズ算出のためだけに参照 */
#include "host_mock.h"
#include "host_diag.h"   /* script_strerror / host_suggest_name */

/* 固定アリーナ（ヒープ不使用。ホストが用意する固定RAM領域に相当, §12） */
static char g_arena[sizeof(script_vm_t) + 128];

int get_vmsize(void){
    return sizeof(g_arena);
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    *out_len = fread(buf, 1, (size_t)sz, f);
    buf[*out_len] = '\0';
    fclose(f);
    return buf;
}

/* 多値イベント post のサンプル（int, int, str）。RXDATA源へ1イベントをアトミックに積む。
 * スクリプト側 ON RXDATA では ARG[0]=src, ARG[1]=dst, SARG[2]=data で読める。 */
static void post_rxdata(void)
{
    static int seq = 0;
    static const char *msgs[] = { "PING", "HELLO", "YAJIR", "BTN-UP" };
    script_arg_t a[3];
    seq++;
    a[0] = SCRIPT_ARG_INT(seq);                  /* src  : 連番 */
    a[1] = SCRIPT_ARG_INT(0x10 + (seq & 0x0F));  /* dst  : 0x10.. */
    a[2] = SCRIPT_ARG_STR(msgs[seq & 3], strlen(msgs[seq & 3]));  /* data : 文字列（位置2 → SARG[2]） */
    script_post_msg_v("RXDATA", 3, a);
}

/* 擬似割り込みドライバ：キーボードを UART1 / BTN / RXDATA イベントへ。戻り値1で終了要求 */
static int poll_pseudo_irq(void)
{
    while (_kbhit()) {
        int ch = _getch();
        if (ch == 27) return 1;                          /* ESC=終了 */
        if (ch == '\t') { script_post_msg("BTN", 0); }   /* TAB=ボタン押下 */
        else if (ch == 'd' || ch == 'D') { post_rxdata(); } /* d=RXDATA（多値: int,int,str） */
        else if (ch == 'm' || ch == 'M') { host_fire_myhandler(1, 2, 3); } /* m=MYHANDLERをC側からpost */
        else if (ch == '\r') { script_post_msg_char("UART1", '\n'); }
        else { script_post_msg_char("UART1", (char)ch); } /* 印字キー=UART受信 */
        /* 実機: ここは UART_RX_IRQHandler / EXTI_IRQHandler / パケット受信完了 から呼ばれる */
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    char *src;
    size_t len = 0;
    int max_ms = -1;
    int32_t start_ms, next_btn_ms;

    setvbuf(stdout, NULL, _IONBF, 0);   /* デバッグ容易化：即時フラッシュ */

    if (argc < 3 || strcmp(argv[1], "run") != 0) {
        fprintf(stderr, "usage: %s run <script.txt> [seconds]\n", argv[0]);
        return 2;
    }
    path = argv[2];
    if (argc >= 4) max_ms = atoi(argv[3]) * 1000;

    src = read_file(path, &len);
    if (!src) { fprintf(stderr, "cannot read file: %s\n", path); return 2; }

    /* init → register（def_*の実体）→ load（受信→コンパイル→常駐, §11） */
    script_init(g_arena, sizeof(g_arena));
    host_register_all();

    if (script_load(src, len) != 0) {
        /* (B)方針：コアは構造化エラーだけ返す。表示文は host 側（host_diag）で組む。 */
        const script_error_t *e = script_last_error();
        fprintf(stderr, "load error: line %d: %s", e->line, script_strerror(e->code));
        if (e->tok[0]) fprintf(stderr, " '%s'", e->tok);
        if (e->code == ERR_END_EXPECTED && e->aux)
            fprintf(stderr, " (block opened at line %d)", e->aux);
        if ((e->code == ERR_UNKNOWN_PORT || e->code == ERR_UNKNOWN_NAME) && e->tok[0]) {
            const char *sug = host_suggest_name(e->tok);
            if (sug) fprintf(stderr, " (did you mean '%s'?)", sug);
        }
        fprintf(stderr, "\n");
        free(src);
        return 1;
    }
    free(src);

    printf("[script] loaded '%s'. keys: <printable>=UART1 echo, TAB=BTN, d=RXDATA(int,int,str), m=MYHANDLER, ESC=quit\n", path);
    if (max_ms >= 0) printf("[script] auto-stop after %d s\n", max_ms / 1000);

    start_ms = get_tick();
    next_btn_ms = start_ms + 3000;

    for (;;) {
        if (poll_pseudo_irq()) break;

        /* 入力が無くてもデモが進むよう、3秒ごとに擬似BTN（BUZZER+TIMERの実演） */
        if (get_tick() >= next_btn_ms) {
            script_post_msg("BTN", 0);
            next_btn_ms += 3000;
        }

        script_tick();   /* タイマ満期＋キュー＋周期＋MAIN を1ステップ（§1, §11） */

        if (max_ms >= 0 && (get_tick() - start_ms) >= max_ms) break;
        Sleep(1);        /* 協調ループの間引き（実機は不要） */
    }

    printf("\n[script] stopped.\n");
    if (script_event_overflow()) printf("[warn] event queue overflowed\n");
    if (script_timer_overflow()) printf("[warn] timer slots overflowed\n");
    return 0;
}
