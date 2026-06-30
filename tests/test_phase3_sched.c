/* test_phase3_sched.c - フェーズ3（スケジューラ）の単体テスト
 *
 * 制御可能なクロック g_now を NOW ポートに束ねて script_tick() を刻み、
 * MAINのWAIT yield/再開、周期ON、INIT 1回実行を検証する。
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

static int32_t g_seq[128]; static int g_n = 0;
static void out_rec(int argc, const script_value_t *a){ g_seq[g_n++] = (argc>0)?a[0].i:0; }

static void tick_at(int32_t t){ g_now = t; script_tick(); }

static void setup(void){
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in("NOW", now_get, SCRIPT_T_INT);
    script_register_out("OUT", out_rec);
}

int main(void)
{
    setup();
    printf("== Phase3 scheduler ==\n");

    /* 1) MAIN の WAIT yield/再開 */
    {
        const char *src =
            "MAIN\n"
            "    1000 -> WAIT\n"
            "    1 -> OUT\n"
            "    1000 -> WAIT\n"
            "    2 -> OUT\n"
            "END\n";
        CHECK(script_load(src, strlen(src)) == 0, "MAIN script loads");
        g_n = 0;
        tick_at(0);     /* 最初のWAITでyield, wake=1000 */
        CHECK(g_n == 0, "nothing before first WAIT elapses");
        tick_at(500);   /* まだ待ち */
        CHECK(g_n == 0, "still waiting at t=500");
        tick_at(1000);  /* 起床→ 1->OUT →次WAIT(wake=2000) */
        CHECK(g_n == 1 && g_seq[0] == 1, "OUT 1 at t=1000");
        tick_at(2000);  /* 起床→ 2->OUT → END(周回) */
        CHECK(g_n == 2 && g_seq[1] == 2, "OUT 2 at t=2000");
        tick_at(2000);  /* 次パス開始：最初のWAITでyield */
        CHECK(g_n == 2, "next pass yields again (no extra output)");
        tick_at(3000);  /* 周回後の 1->OUT */
        CHECK(g_n == 3 && g_seq[2] == 1, "loops: OUT 1 again at t=3000");
    }

    /* 2) 周期 ON 100 と INIT 1回 */
    {
        const char *src =
            "INIT\n"
            "    7 -> OUT\n"        /* 起動時1回だけ */
            "END\n"
            "ON 100\n"
            "    9 -> OUT\n"
            "END\n";
        CHECK(script_load(src, strlen(src)) == 0, "INIT+periodic loads");
        g_n = 0;
        tick_at(0);     /* INIT(7) 実行。next_time=100。now<100で周期未発火 */
        CHECK(g_n == 1 && g_seq[0] == 7, "INIT runs once at first tick");
        tick_at(50);
        CHECK(g_n == 1, "periodic not due at t=50");
        tick_at(100);
        CHECK(g_n == 2 && g_seq[1] == 9, "periodic fires at t=100");
        tick_at(150);
        CHECK(g_n == 2, "no double-fire at t=150");
        tick_at(250);
        CHECK(g_n == 3 && g_seq[2] == 9, "periodic fires again by t=250");
        tick_at(250);
        CHECK(g_n == 3, "INIT does not run again");
    }

    /* 3) 周期ON（既定 CATCHUP＝回数尊重）：取りこぼしを欠落させず1 tickにつき1回ずつ消化 */
    {
        const char *src = "ON 100\n    NOW -> OUT\nEND\n";  /* 発火時のNOWを記録 */
        CHECK(script_load(src, strlen(src)) == 0, "periodic catchup script loads");
        g_n = 0;
        tick_at(0);                 /* setup: next_time=100 */
        tick_at(100);               /* fire(100), next=200 */
        CHECK(g_n == 1 && g_seq[0] == 100, "on-cadence fire at 100");

#if TS_PERIODIC_CATCHUP
        /* 大ストールで now=500（満期 200/300/400/500 が溜まる）。
         * バーストせず 1 tick 1回ずつ消化し、回数を欠落させない。 */
        tick_at(500);
        CHECK(g_n == 2 && g_seq[1] == 500, "stall: fires ONCE this tick (no burst)");
        tick_at(500);               /* deadline 300 を消化 */
        tick_at(500);               /* deadline 400 を消化 */
        tick_at(500);               /* deadline 500 を消化 → next=600 */
        CHECK(g_n == 5, "backlog drained one-per-tick (200/300/400/500 all fired)");
        tick_at(500);
        CHECK(g_n == 5, "no over-fire once caught up (next=600 > 500)");
        tick_at(600);
        CHECK(g_n == 6 && g_seq[5] == 600, "back on cadence at 600");
#else
        /* SMOOTH では取りこぼしを畳むため、stall で1回だけ発火しグリッドへ即復帰 */
        tick_at(500);
        CHECK(g_n == 2 && g_seq[1] == 500, "smooth: coalesced single fire at stall");
        tick_at(500);
        CHECK(g_n == 2, "smooth: backlog skipped (no further fire at same now)");
        tick_at(600);
        CHECK(g_n == 3 && g_seq[2] == 600, "smooth: back on grid at 600");
#endif
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE3 FAILED" : "PHASE3 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
