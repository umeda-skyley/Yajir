/* test_phase4_events.c - フェーズ4（イベント系＋TIMER）の単体テスト
 *
 * post_msg/post_msg_char→キュー→ON ディスパッチ、ARG型タグ、TIMERワンショット、
 * イベントキュー・オーバーフローフラグを検証する。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static int32_t g_now = 0;
static int32_t now_get(void){ return g_now; }

/* OUT は値＋型タグを記録 */
static int32_t g_val[128]; static int g_tag[128]; static int g_n = 0;
static void out_rec(int argc, const script_value_t *a){
    if (argc>0){ g_val[g_n]=a[0].i; g_tag[g_n]=a[0].tag; g_n++; } }

static void tick_at(int32_t t){ g_now=t; script_tick(); }

static void setup(void){
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in ("NOW", now_get, SCRIPT_T_INT);
    script_register_out("OUT", out_rec);
    script_register_handler("UART1");
    script_register_handler("BTN");
}

int main(void)
{
    setup();
    printf("== Phase4 events + TIMER ==\n");

    /* 1) post_msg_char → ON UART1 で ARG[0] を char タグのままエコー（§10） */
    {
        const char *src = "ON UART1\n    ARG[0] -> OUT\nEND\n";
        CHECK(script_load(src, strlen(src)) == 0, "ON UART1 loads");
        g_n = 0;
        tick_at(0);                              /* INIT等なし */
        CHECK(script_post_msg_char("UART1", 'A') == 0, "post_msg_char queued");
        tick_at(1);
        CHECK(g_n == 1 && g_val[0] == 'A' && g_tag[0] == SV_CHAR,
              "ARG[0] echoed with CHAR tag");
    }

    /* 2) post_msg（int） → ARG は int タグ */
    {
        const char *src = "ON UART1\n    ARG[0] -> OUT\nEND\n";
        script_load(src, strlen(src));
        g_n = 0; tick_at(0);
        script_post_msg("UART1", 1234);
        tick_at(1);
        CHECK(g_n == 1 && g_val[0] == 1234 && g_tag[0] == SV_INT, "post_msg int -> ARG int tag");
    }

    /* 3) TIMER ワンショット：ON BTN で 300->TIMER、満期で ON TIMER（§8） */
    {
        const char *src =
            "ON BTN\n"
            "    300 -> TIMER\n"
            "END\n"
            "ON TIMER\n"
            "    42 -> OUT\n"
            "END\n";
        CHECK(script_load(src, strlen(src)) == 0, "TIMER script loads");
        g_n = 0;
        tick_at(0);
        script_post_msg("BTN", 0);
        tick_at(0);                 /* ON BTN 実行→ fire_time=300 */
        CHECK(g_n == 0, "no TIMER fire yet at t=0");
        tick_at(100);
        CHECK(g_n == 0, "no TIMER fire at t=100");
        tick_at(300);               /* 満期→ON TIMER */
        CHECK(g_n == 1 && g_val[0] == 42, "ON TIMER fires once at t=300");
        tick_at(600);               /* ワンショット：再発火しない */
        CHECK(g_n == 1, "one-shot TIMER does not refire");
    }

    /* 4) イベントキュー・オーバーフロー（§10） */
    {
        const char *src = "ON UART1\n    ARG[0] -> OUT\nEND\n";
        int ok = 0, ng = 0, i;
        script_load(src, strlen(src));
        tick_at(0);   /* INIT→RUNへ（v0.3.4: INIT中のpostは破棄されるため先に始動） */
        for (i = 0; i < CFG_EVENT_QUEUE_LEN + 8; i++) {
            if (script_post_msg("UART1", i) == 0) ok++; else ng++;
        }
        CHECK(ng > 0 && script_event_overflow() == 1, "queue overflow sets flag, drops new");
        printf("        (accepted=%d dropped=%d)\n", ok, ng);
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE4 FAILED" : "PHASE4 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
