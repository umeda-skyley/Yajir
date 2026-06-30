/* test_phase9_lifecycle.c - フェーズA（INIT 2フェーズ・ライフサイクル, v0.3.4）
 *
 * INITフェーズ（世界凍結）→ t0 → RUNフェーズ を検証:
 *   - WAIT を INIT で解禁。INIT WAIT中は周期/ハンドラ/MAINが動かない（凍結）。
 *   - 周期ONの初回・INITで張ったTIMERの締切が t0 起点（長いINIT WAITでcatch-up債務なし）。
 *   - INIT中の post は破棄（フラグも立てない）。
 *   - 再ロードは INITフェーズに入り直す。
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
static int32_t g_seq[64]; static int g_n = 0;
static void out_rec(int argc, const script_value_t *a){ if(argc>0) g_seq[g_n++]=a[0].i; }
static void tick_at(int32_t t){ g_now=t; script_tick(); }
static int compile(const char *s){ return script_load(s, strlen(s)); }

static void setup(void){
    static char arena[sizeof(script_vm_t)+128];
    script_init(arena, sizeof(arena));
    script_register_in ("NOW", now_get, SCRIPT_T_INT);
    script_register_out("OUT", out_rec);
    script_register_handler("RX");
}

int main(void)
{
    setup();
    printf("== Phase9 INIT two-phase lifecycle ==\n");

    /* 1) WAIT in INIT＋世界凍結＋周期は t0 起点（catch-up債務なし） */
    {
        const char *src =
            "INIT\n"
            "    100 -> OUT\n"      /* INIT開始で出力 */
            "    5000 -> WAIT\n"    /* INITで5秒待つ（その間ずっと凍結） */
            "    200 -> OUT\n"      /* 起床後＝INIT終了直前 */
            "END\n"
            "ON 1000\n"
            "    9 -> OUT\n"
            "END\n";
        CHECK(compile(src)==0, "INIT-with-WAIT script compiles");
        g_n=0;
        tick_at(0);
        CHECK(g_n==1 && g_seq[0]==100, "INIT runs to its WAIT, then freezes");
        tick_at(1000); CHECK(g_n==1, "frozen: periodic ON 1000 does NOT fire during INIT WAIT");
        tick_at(3000); CHECK(g_n==1, "still frozen at t=3000");
        tick_at(5000);
        CHECK(g_n==2 && g_seq[1]==200, "INIT resumes after WAIT and exits (t0=5000)");
        tick_at(5999); CHECK(g_n==2, "periodic not due before t0+1000");
        tick_at(6000);
        CHECK(g_n==3 && g_seq[2]==9, "periodic first fires at t0+1000=6000 (no catch-up debt)");
    }

    /* 2) INITで張った TIMER は t0 起点で満期 */
    {
        const char *src =
            "INIT\n"
            "    300 -> TIMER\n"    /* INITで張る → t0+300 */
            "    2000 -> WAIT\n"
            "    1 -> OUT\n"
            "END\n"
            "ON TIMER\n"
            "    7 -> OUT\n"
            "END\n";
        CHECK(compile(src)==0, "INIT-TIMER script compiles");
        g_n=0;
        tick_at(0);                 /* TIMER張る（相対300）, WAIT */
        tick_at(300);  CHECK(g_n==0, "INIT timer does NOT fire at arm+300 (still INIT)");
        tick_at(2000); CHECK(g_n==1 && g_seq[0]==1, "INIT exits at t0=2000");
        tick_at(2299); CHECK(g_n==1, "timer not due before t0+300");
        tick_at(2300); CHECK(g_n==2 && g_seq[1]==7, "ON TIMER fires at t0+300=2300");
    }

    /* 3) INIT中の post は破棄（フラグも立てない） */
    {
        const char *src =
            "INIT\n"
            "    1000 -> WAIT\n"
            "    5 -> OUT\n"
            "END\n"
            "ON RX\n"
            "    ARG[0] -> OUT\n"
            "END\n";
        compile(src); g_n=0;
        tick_at(0);                 /* INIT WAIT中 */
        CHECK(script_post_msg("RX", 42)==0, "post during INIT returns 0 (accepted-as-dropped)");
        tick_at(500);
        tick_at(1000);              /* INIT終了→RUN */
        CHECK(g_n==1 && g_seq[0]==5, "INIT-time RX(42) was dropped (only INIT marker 5)");
        CHECK(script_event_overflow()==0, "drop sets no overflow flag");
        script_post_msg("RX", 99); /* RUNフェーズの post は通る */
        tick_at(1001);
        CHECK(g_n==2 && g_seq[1]==99, "RUN-phase RX(99) is delivered");
    }

    /* 4) 再ロードは INITフェーズに入り直す（INITが再実行される） */
    {
        const char *src = "INIT\n    7 -> OUT\nEND\n";
        compile(src); g_n=0; tick_at(0);
        CHECK(g_n==1 && g_seq[0]==7, "INIT runs once after first load");
        compile(src);              /* 再ロード */
        tick_at(1);
        CHECK(g_n==2 && g_seq[1]==7, "reload re-enters INIT (runs again)");
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE9 FAILED" : "PHASE9 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
