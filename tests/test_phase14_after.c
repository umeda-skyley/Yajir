/* test_phase14_after.c - フェーズ14（遅延post `値 -> ハンドラ AFTER <ms>`）の単体テスト（v0.4.7 §10）
 *
 * 遅延発火・引数の持ち回り（int/str）・post時スナップショット・ms<=0 の即post縮退・
 * INIT からの予約（t0起点解決）・pending 満杯（ERR_DELAY_FULL）・位置違反/型違反を検証。
 * 時刻は NOW ポート（host_mock のモッククロック）に依存するため、tick を回して進める。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static int compile(const char *s){ return script_load(s, strlen(s)); }

/* モッククロック（NOW ポートの実体）。テストから任意に進める。 */
static int32_t g_now = 0;
static int32_t now_get(void) { return g_now; }
/* n tick 回す（1 tick = step ms 進める） */
static void run_ticks(int n, int32_t step) { int i; for (i=0;i<n;i++){ script_tick(); g_now += step; } }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in("NOW", now_get, SCRIPT_T_INT);   /* 内部クロックを差し替え */
    printf("== Phase14 delayed post (AFTER) ==\n");

    /* 1) 基本: 100ms後に発火。それ以前は発火しない */
    {
        const char *src =
            "def_handler(LATER)\n"
            "INIT\n"
            "    0 -> GVAR[0]\n"
            "END\n"
            "MAIN\n"
            "    (GVAR[1] == 0) -> IFYES\n"
            "        1 -> GVAR[1]\n"
            "        7 -> LATER AFTER 100\n"       /* 一度だけ張る */
            "    END\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON LATER\n"
            "    ARG[0] -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "AFTER compiles");
        if (r==0){
            g_now = 0;
            run_ticks(3, 10);                      /* t=0..30ms: まだ */
            CHECK(vm()->gvar[0].i == 0, "not fired before the delay elapses");
            run_ticks(12, 10);                     /* t>100ms: 発火 */
            CHECK(vm()->gvar[0].i == 7, "fired after 100ms with ARG[0]==7");
        }
    }

    /* 2) 引数を持ち回る（int複数＋str）＋ post時スナップショット */
    {
        const char *src =
            "def_handler(PAY)\n"
            "MAIN\n"
            "    (GVAR[3] == 0) -> IFYES\n"
            "        1 -> GVAR[3]\n"
            "        \"snap\" -> SVAR[0]\n"
            "        SVAR[0], 11, 22 -> PAY AFTER 50\n"   /* SVAR[0] の中身を post 時にコピー */
            "        \"CHANGED\" -> SVAR[0]\n"            /* 満期前に元を書き換える */
            "    END\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON PAY\n"
            "    SARG[0] -> SGVAR[0]\n"
            "    ARG[1] -> GVAR[0]\n"
            "    ARG[2] -> GVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "AFTER with mixed args compiles");
        if (r==0){
            g_now = 0;
            run_ticks(12, 10);
            CHECK(vm()->gvar[0].i == 11 && vm()->gvar[1].i == 22, "int args carried (11, 22)");
            CHECK(strcmp(vm()->sgvar[0], "snap") == 0, "str arg is a post-time snapshot ('snap')");
        }
    }

    /* 3) AFTER 0 / 負値 → 即post に縮退（次tickで発火） */
    {
        const char *src =
            "def_handler(NOWISH)\n"
            "MAIN\n"
            "    (GVAR[3] == 0) -> IFYES\n"
            "        1 -> GVAR[3]\n"
            "        5 -> NOWISH AFTER 0\n"
            "        6 -> NOWISH AFTER -100\n"
            "    END\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON NOWISH\n"
            "    GVAR[0] + ARG[0] -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "AFTER 0 / negative compiles");
        if (r==0){
            g_now = 0;
            run_ticks(3, 10);                      /* 遅延せず即座に（次tickで）両方発火 */
            CHECK(vm()->gvar[0].i == 11, "AFTER <=0 degrades to immediate post (5+6)");
        }
    }

    /* 4) INIT からの予約（t0起点で解決・破棄されない） */
    {
        const char *src =
            "def_handler(KICKOFF)\n"
            "INIT\n"
            "    42 -> KICKOFF AFTER 100\n"        /* INIT中に張る＝t0起点 */
            "END\n"
            "MAIN\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON KICKOFF\n"
            "    ARG[0] -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "AFTER in INIT compiles");
        if (r==0){
            g_now = 0;
            run_ticks(3, 10);
            CHECK(vm()->gvar[0].i == 0, "INIT-armed delay not fired yet at t0+30ms");
            run_ticks(12, 10);
            CHECK(vm()->gvar[0].i == 42, "INIT-armed delay fires at t0+100ms (not discarded)");
        }
    }

    /* 5) pending 満杯 → ERR_DELAY_FULL（CFG_DELAY_SLOTS=4 に対し5本張る） */
    {
        const char *src =
            "def_handler(Q)\n"
            "MAIN\n"
            "    (GVAR[3] == 0) -> IFYES\n"
            "        1 -> GVAR[3]\n"
            "        1 -> Q AFTER 500\n"
            "        2 -> Q AFTER 500\n"
            "        3 -> Q AFTER 500\n"
            "        4 -> Q AFTER 500\n"
            "        5 -> Q AFTER 500\n"           /* 5本目＝満杯 */
            "    END\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON Q\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "5 pending delays compile");
        if (r==0){
            g_now = 0;
            script_clear_status(ERR_DELAY_FULL);
            run_ticks(2, 10);
            CHECK((script_get_status() & ERR_DELAY_FULL) != 0, "pending full -> ERR_DELAY_FULL");
            run_ticks(60, 10);
            CHECK(vm()->gvar[0].i == CFG_DELAY_SLOTS, "only CFG_DELAY_SLOTS delays fired (newest dropped)");
        }
    }

    /* 6) スロットは満期で解放され再利用できる */
    {
        const char *src =
            "def_handler(R)\n"
            "MAIN\n"
            "    (GVAR[3] < 8) -> IFYES\n"
            "        GVAR[3] + 1 -> GVAR[3]\n"
            "        1 -> R AFTER 20\n"            /* 毎回1本張る→満期で解放される */
            "    END\n"
            "    30 -> WAIT\n"
            "END\n"
            "ON R\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "slot reuse case compiles");
        if (r==0){
            g_now = 0;
            script_clear_status(ERR_DELAY_FULL);
            run_ticks(80, 10);
            CHECK(vm()->gvar[0].i == 8, "8 sequential delays all fired (slots recycled)");
            CHECK((script_get_status() & ERR_DELAY_FULL) == 0, "no ERR_DELAY_FULL when slots recycle");
        }
    }

    /* ---- エラー系 ---- */

    /* 7) AFTER をハンドラ以外に付ける → ERR_BAD_POSITION（SEED=組込みの out ポート） */
    {
        int r = compile("MAIN\n    1 -> SEED AFTER 100\n    1000 -> WAIT\nEND\n");
        CHECK(r == ERR_BAD_POSITION, "AFTER on an out port -> ERR_BAD_POSITION");
    }
    /* inout ポートに付けても同じ */
    {
        int r = compile("MAIN\n    1, 0, 9 -> CLAMP AFTER 100\n    1000 -> WAIT\nEND\n");
        CHECK(r == ERR_BAD_POSITION, "AFTER on an inout port -> ERR_BAD_POSITION");
    }
    {
        int r = compile("MAIN\n    1 -> GVAR[0] AFTER 100\n    1000 -> WAIT\nEND\n");
        CHECK(r == ERR_BAD_POSITION, "AFTER on a slot -> ERR_BAD_POSITION");
    }

    /* 8) 遅延が str → ERR_TYPE_MISMATCH */
    {
        int r = compile("def_handler(H)\nMAIN\n    1 -> H AFTER \"x\"\n    1000 -> WAIT\nEND\n");
        CHECK(r == ERR_TYPE_MISMATCH, "string delay -> ERR_TYPE_MISMATCH");
    }

    /* 9) 遅延は式でよい（変数・演算） */
    {
        const char *src =
            "def_handler(E)\n"
            "def_alias(UNIT, 40)\n"
            "MAIN\n"
            "    def_local(D, VAR[0])\n"
            "    (GVAR[3] == 0) -> IFYES\n"
            "        1 -> GVAR[3]\n"
            "        2 -> D\n"
            "        9 -> E AFTER UNIT * D\n"      /* 80ms */
            "    END\n"
            "    1000000 -> WAIT\n"
            "END\n"
            "ON E\n"
            "    ARG[0] -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "expression delay compiles");
        if (r==0){
            g_now = 0;
            run_ticks(4, 10);
            CHECK(vm()->gvar[0].i == 0, "expr delay (80ms) not fired at 40ms");
            run_ticks(8, 10);
            CHECK(vm()->gvar[0].i == 9, "expr delay fired after 80ms");
        }
    }

    printf("\n%s (fails=%d)\n", g_fail==0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail ? 1 : 0;
}
