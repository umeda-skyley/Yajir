/* test_phase18_argc.c - フェーズ18（ARGC＝受け取った引数の個数）の単体テスト（v0.4.12 §10）
 *
 * `none -> H`（argc=0）と `0 -> H`（argc=1・値0）は別物だが、ARG[k] が不足位置を benign 0 で
 * 返すため、従来スクリプトからは区別できなかった（C の out ポートは argc で区別できていた＝非対称）。
 * ARGC はその非対称を埋める組み込み入力ポート。ここでは:
 *   (1) none と 0 を区別できる
 *   (2) 引数を受け取らない文脈（INIT/MAIN/周期ON/ON TIMER）は 0
 *       ※ fill_args はイベント経路でしか呼ばれないので、ここが素朴実装だと
 *          「前に発火した別ハンドラの個数」が漏れる。その回帰を防ぐのが主目的。
 *   (3) PORT 呼び出しで private（呼び先の個数へ切替→戻ると caller の値へ復帰）
 * を検証する。
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

static int32_t g_now = 0;
static int32_t now_get(void){ return g_now; }
static void run_ticks(int n){ int i; for (i=0;i<n;i++){ script_tick(); g_now += 10; } }

/* C 側 out ポート: argc をそのまま記録（コア側の argc と突き合わせる） */
static int g_last_argc = -1;
static void out_argc(int argc, const script_value_t *a){ (void)a; g_last_argc = argc; }

static void setup(char *arena, size_t sz)
{
    script_init(arena, sz);
    script_register_now(now_get);
    script_register_out("OUTA", out_argc);
    g_last_argc = -1;
    g_now = 0;
}

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    printf("== Phase18 ARGC (argument count seen by the receiver) ==\n");

    /* 1) C の out ポートは元から argc を区別できる（ARGC の意味の基準） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    none -> OUTA\nEND\n") == 0, "none -> OUTA compiles");
        run_ticks(1);
        CHECK(g_last_argc == 0, "C out port sees argc==0 for 'none'");

        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    0 -> OUTA\nEND\n") == 0, "0 -> OUTA compiles");
        run_ticks(1);
        CHECK(g_last_argc == 1, "C out port sees argc==1 for '0'");
    }

    /* 2) 本題: スクリプト側（ON ハンドラ）でも none と 0 を区別できる */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_handler(H1)\n"
                      "def_handler(H2)\n"
                      "INIT\n"
                      "    none -> H1 AFTER 10\n"      /* INIT からの即post は橋で捨てられる＝AFTER */
                      "    0    -> H2 AFTER 30\n"
                      "END\n"
                      "ON H1\n"
                      "    ARGC -> GVAR[0]\n"
                      "END\n"
                      "ON H2\n"
                      "    ARGC -> GVAR[1]\n"
                      "END\n") == 0, "ARGC script compiles");
        run_ticks(8);
        CHECK(vm()->gvar[0].i == 0, "ON H1: ARGC==0 for 'none'");
        CHECK(vm()->gvar[1].i == 1, "ON H2: ARGC==1 for '0'  <- none/0 are distinguishable now");
    }

    /* 3) 多値: 受け取った位置の個数を返す */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_handler(H3)\n"
                      "INIT\n    1, 2, 3 -> H3 AFTER 10\nEND\n"
                      "ON H3\n    ARGC -> GVAR[0]\nEND\n") == 0, "multi-arg compiles");
        run_ticks(5);
        CHECK(vm()->gvar[0].i == 3, "ARGC==3 for three positions");
    }

    /* 4) 引数を受け取らない文脈は 0（INIT / MAIN / 周期ON）。
     *    ★回帰防止の要: fill_args はイベント経路でしか呼ばれないので、素朴実装だと
     *      直前に発火したハンドラの個数が漏れて 3 などになる。 */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_handler(H4)\n"
                      "INIT\n"
                      "    ARGC -> GVAR[0]\n"          /* INIT: 0 */
                      "    1, 2, 3 -> H4 AFTER 10\n"   /* わざと argc=3 のイベントを先に流す */
                      "END\n"
                      "ON H4\n"
                      "    ARGC -> GVAR[1]\n"          /* ハンドラ: 3 */
                      "END\n"
                      "ON 50\n"
                      "    ARGC -> GVAR[2]\n"          /* 周期ON: 0（3 が漏れないこと） */
                      "END\n"
                      "MAIN\n"
                      "    ARGC -> GVAR[3]\n"          /* MAIN: 0 */
                      "    1000 -> WAIT\n"
                      "END\n") == 0, "mixed-context script compiles");
        run_ticks(12);
        CHECK(vm()->gvar[0].i == 0, "INIT: ARGC==0");
        CHECK(vm()->gvar[1].i == 3, "ON handler: ARGC==3");
        CHECK(vm()->gvar[2].i == 0, "periodic ON: ARGC==0 (no leak from the handler)");
        CHECK(vm()->gvar[3].i == 0, "MAIN: ARGC==0");
    }

    /* 5) ON TIMER も引数なし＝0 */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    20 -> TIMER\nEND\n"
                      "ON TIMER\n    ARGC + 100 -> GVAR[0]\nEND\n") == 0, "ON TIMER compiles");
        run_ticks(8);
        CHECK(vm()->gvar[0].i == 100, "ON TIMER: ARGC==0");
    }

    /* 6) PORT 呼び出しで private: 呼び先は自分の個数、戻ると caller の値へ復帰。
     *    ★これを忘れると「サブルーチンを呼ぶと ARGC が変わる」バグになる。 */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_handler(H5)\n"
                      "def_port(SUB, T_INT)\n"
                      "PORT SUB\n"
                      "    ARGC -> EXIT\n"             /* 呼び先が受け取った個数を返す */
                      "END\n"
                      "INIT\n    7, 8 -> H5 AFTER 10\nEND\n"
                      "ON H5\n"
                      "    ARGC -> GVAR[0]\n"          /* 呼ぶ前: 2 */
                      "    1, 2, 3 -> SUB -> GVAR[1]\n"/* 呼び先: 3 */
                      "    ARGC -> GVAR[2]\n"          /* 戻った後: 2 に復帰していること */
                      "END\n") == 0, "PORT + ARGC compiles");
        run_ticks(6);
        CHECK(vm()->gvar[0].i == 2, "before call: ARGC==2 (handler's own)");
        CHECK(vm()->gvar[1].i == 3, "inside PORT: ARGC==3 (callee's own)");
        CHECK(vm()->gvar[2].i == 2, "after return: ARGC restored to 2  <- private, like ARG/VAR");
    }

    /* 7) 引数0で呼んだ PORT は ARGC==0（値源として読む＝`SUB` 単独読み） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_port(SUB2, T_INT)\n"
                      "PORT SUB2\n    ARGC -> EXIT\nEND\n"
                      "INIT\n    SUB2 -> GVAR[0]\nEND\n") == 0, "zero-arg PORT read compiles");
        run_ticks(2);
        CHECK(vm()->gvar[0].i == 0, "PORT read as a value source: ARGC==0");
    }

    /* 8) ARGC は読み取り専用（送り先には立てられない） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    1 -> ARGC\nEND\n") != 0, "ARGC cannot be a send target");
    }

    printf(g_fail ? "PHASE18 FAILED (failures=%d)\n" : "PHASE18 PASSED (failures=%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
