/* test_phase11_loops.c - フェーズ11（REPEAT / ITR / 動的添字）の単体テスト（v0.4.4 §4, §7）
 *
 * 有界ループ REPEAT と反復カウンタ ITR、スロットの動的添字アクセス（read/write・int/str・
 * 範囲外benign）、ネスト、N<=0、バジェット打ち切り(ERR_BUDGET)、および各種コンパイルエラーを検証。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static void run_init(void){
    int i; for (i=0;i<vm()->nblocks;i++) if (vm()->blocks[i].kind==BLK_INIT){
        uint16_t pc=vm()->blocks[i].bc_start; int b=CFG_INSTR_BUDGET; int32_t ms=0;
        vm()->sp=0; vm()->loop_sp=0; vm_exec(&pc,&b,&ms,0); return; }
}
static int compile(const char *s){ return script_load(s, strlen(s)); }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    printf("== Phase11 REPEAT / ITR / dynamic index ==\n");

    /* 1) REPEAT + ITR: 1..10 を合計 = 55 */
    {
        const char *src =
            "INIT\n"
            "    0 -> GVAR[0]\n"
            "    10 -> REPEAT\n"
            "        GVAR[0] + ITR -> GVAR[0]\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "REPEAT/ITR compiles");
        if (r==0){ run_init(); CHECK(vm()->gvar[0].i == 55, "sum ITR 1..10 == 55"); }
    }

    /* 2) 動的ライト（int）: VAR[ITR-1]=ITR^2 を埋め、動的リードで取り出す */
    {
        const char *src =
            "INIT\n"
            "    5 -> REPEAT\n"
            "        ITR, ITR * ITR -> VAR\n"     /* VAR[ITR-1] = ITR^2 */
            "    END\n"
            "    3 -> VAR -> GVAR[0]\n"           /* read VAR[2]=9（チェイン） */
            "    5 -> VAR -> GVAR[1]\n"           /* read VAR[4]=25 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "dynamic write/read (int) compiles");
        if (r==0){ run_init();
            CHECK(vm()->var[0].i == 1,   "VAR[0] == 1^2");
            CHECK(vm()->var[4].i == 25,  "VAR[4] == 5^2");
            CHECK(vm()->gvar[0].i == 9,  "3 -> VAR -> GVAR[0] == 9");
            CHECK(vm()->gvar[1].i == 25, "5 -> VAR -> GVAR[1] == 25");
        }
    }

    /* 3) 動的ライトの産出（書いた値を産出）＋範囲外benign */
    {
        const char *src =
            "INIT\n"
            "    5, 1000 -> VAR -> GVAR[0]\n"     /* VAR[4]=1000、書いた値1000を産出 */
            "    100 -> VAR -> GVAR[1]\n"         /* 範囲外read → 0 */
            "    100, 7 -> VAR -> GVAR[2]\n"      /* 範囲外write → no-op, 産出0 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "dynamic write-produce/OOR compiles");
        if (r==0){ run_init();
            CHECK(vm()->var[4].i == 1000, "5,1000 -> VAR writes VAR[4]");
            CHECK(vm()->gvar[0].i == 1000, "write produces written value");
            CHECK(vm()->gvar[1].i == 0,   "OOR read -> 0 (benign)");
            CHECK(vm()->gvar[2].i == 0,   "OOR write -> 0 (benign)");
        }
    }

    /* 4) 動的添字（str）: SVAR write/read */
    {
        const char *src =
            "INIT\n"
            "    1, \"hello\" -> SVAR\n"          /* SVAR[0]="hello" */
            "    2, \"world\" -> SVAR\n"          /* SVAR[1]="world" */
            "    1 -> SVAR -> SGVAR[0]\n"         /* read SVAR[0] → SGVAR[0] */
            "    2 -> SVAR -> SGVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "dynamic str write/read compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->svar[0], "hello")==0, "SVAR[0] == hello");
            CHECK(strcmp(vm()->sgvar[0],"hello")==0, "1 -> SVAR -> SGVAR[0] == hello");
            CHECK(strcmp(vm()->sgvar[1],"world")==0, "2 -> SVAR -> SGVAR[1] == world");
        }
    }

    /* 5) ネスト REPEAT: 3×2 = 6 回 */
    {
        const char *src =
            "INIT\n"
            "    0 -> GVAR[0]\n"
            "    3 -> REPEAT\n"
            "        2 -> REPEAT\n"
            "            GVAR[0] + 1 -> GVAR[0]\n"
            "        END\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "nested REPEAT compiles");
        if (r==0){ run_init(); CHECK(vm()->gvar[0].i == 6, "nested 3x2 == 6"); }
    }

    /* 6) N<=0 は本体スキップ */
    {
        const char *src =
            "INIT\n"
            "    5 -> GVAR[0]\n"
            "    0 -> REPEAT\n"
            "        99 -> GVAR[0]\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "REPEAT N=0 compiles");
        if (r==0){ run_init(); CHECK(vm()->gvar[0].i == 5, "N<=0 skips body"); }
    }

    /* 7) バジェット打ち切り: 巨大 REPEAT → ERR_BUDGET・未完 */
    {
        const char *src =
            "INIT\n"
            "    0 -> GVAR[0]\n"
            "    1000000 -> REPEAT\n"
            "        GVAR[0] + 1 -> GVAR[0]\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "huge REPEAT compiles");
        if (r==0){
            script_clear_status(ERR_BUDGET);
            run_init();
            CHECK((script_get_status() & ERR_BUDGET) != 0, "huge REPEAT sets ERR_BUDGET");
            CHECK(vm()->gvar[0].i < 1000000, "huge REPEAT aborted (not completed)");
            script_clear_status(ERR_BUDGET);
        }
    }

    /* 8) コンパイルエラー群 */
    {
        CHECK(compile("MAIN\n 3 -> REPEAT\n 10 -> WAIT\n END\nEND\n") != 0, "WAIT in REPEAT = error");
        CHECK(compile("INIT\n ITR -> GVAR[0]\nEND\n")               != 0, "ITR outside REPEAT = error");
        CHECK(compile("INIT\n 1, 2 -> ARG\nEND\n")                  != 0, "dynamic write to ARG = error");
        CHECK(compile("INIT\n 1, \"x\" -> VAR\nEND\n")              != 0, "str value to int slot = error");
        CHECK(compile("INIT\n \"x\" -> VAR\nEND\n")                 != 0, "str index = error");
        CHECK(compile("INIT\n 1 -> REPEAT\n1 -> REPEAT\n1 -> REPEAT\n1 -> REPEAT\n1 -> REPEAT\n"
                      "1 -> GVAR[0]\nEND\nEND\nEND\nEND\nEND\nEND\n") != 0, "REPEAT nest > 4 = error");
    }

    /* 9) 動的リードで ARG は許可（受信専用でも read はOK） */
    {
        const char *src = "INIT\n 1 -> ARG -> GVAR[0]\nEND\n";
        CHECK(compile(src) == 0, "dynamic read of ARG allowed");
    }

    printf(g_fail ? "PHASE11 FAILED (failures=%d)\n" : "PHASE11 PASSED (failures=0)\n", g_fail);
    return g_fail ? 1 : 0;
}
