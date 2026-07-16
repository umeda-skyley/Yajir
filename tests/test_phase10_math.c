/* test_phase10_math.c - フェーズ10（数値ユーティリティ）の単体テスト（v0.4.3 §3, §9）
 *
 * RAND/SEED（xorshift32・非負・再現性）、CLAMP、MAP（線形写像・0除算）、MIN/MAX、ABS を検証する。
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
        vm()->sp=0; vm_exec(&pc,&b,&ms,0); return; }
}
static int32_t t_now(void){ return 0; }   /* v0.4.8: ロード時クロック要求を満たす最小スタブ */
static int compile(const char *s){ return script_load(s, strlen(s)); }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_now(t_now);   /* v0.4.8: クロック源が無いと script_load が ERR_NO_CLOCK */
    printf("== Phase10 numeric utilities ==\n");

    /* 1) RAND: 非負・前進・SEED 再現性（スクリプト側 SEED） */
    {
        const char *src =
            "INIT\n"
            "    12345 -> SEED\n"
            "    RAND -> GVAR[0]\n"     /* 1回目 */
            "    RAND -> GVAR[1]\n"     /* 2回目（前進） */
            "    12345 -> SEED\n"       /* 同じ種で張り直し */
            "    RAND -> GVAR[2]\n"     /* → 1回目と一致するはず */
            "    RAND % 100 -> GVAR[3]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "RAND/SEED script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i >= 0 && vm()->gvar[1].i >= 0, "RAND is non-negative");
            CHECK(vm()->gvar[0].i != vm()->gvar[1].i, "RAND advances (two draws differ)");
            CHECK(vm()->gvar[2].i == vm()->gvar[0].i, "SEED reseed reproduces first draw");
            CHECK(vm()->gvar[3].i >= 0 && vm()->gvar[3].i < 100, "RAND % 100 in [0,100)");
        }
    }

    /* 2) script_srand（C-API）: 同じ種で同じ列・別種で別列 */
    {
        const char *src = "INIT\n    RAND -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == 0, "srand script compiles");
        if (r==0){
            int32_t v0, v1;
            script_srand(42); run_init(); v0 = vm()->gvar[0].i;
            script_srand(42); run_init(); v1 = vm()->gvar[0].i;
            CHECK(v0 == v1, "script_srand(42) reproducible");
            CHECK(v0 >= 0,  "script_srand draw non-negative");
            script_srand(43); run_init();
            CHECK(vm()->gvar[0].i != v0, "different seed -> different draw");
        }
    }

    /* 3) CLAMP: 範囲内/下限/上限 */
    {
        const char *src =
            "INIT\n"
            "    50, 0, 100 -> CLAMP\n"
            "    RESULT -> GVAR[0]\n"     /* 50 */
            "    -20, 0, 100 -> CLAMP\n"
            "    RESULT -> GVAR[1]\n"     /* 0 */
            "    130, 0, 100 -> CLAMP\n"
            "    RESULT -> GVAR[2]\n"     /* 100 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "CLAMP script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i == 50,  "CLAMP 50 in [0,100] -> 50");
            CHECK(vm()->gvar[1].i == 0,   "CLAMP -20 -> 0 (low)");
            CHECK(vm()->gvar[2].i == 100, "CLAMP 130 -> 100 (high)");
        }
    }

    /* 4) MAP: 端点・中点・別レンジ */
    {
        const char *src =
            "INIT\n"
            "    2048, 0, 4095, 0, 100 -> MAP\n"
            "    RESULT -> GVAR[0]\n"     /* 50（204800/4095=50.0..） */
            "    0, 0, 10, 100, 200 -> MAP\n"
            "    RESULT -> GVAR[1]\n"     /* 100（inLo→outLo） */
            "    10, 0, 10, 100, 200 -> MAP\n"
            "    RESULT -> GVAR[2]\n"     /* 200（inHi→outHi） */
            "    5, 0, 10, 0, 100 -> MAP\n"
            "    RESULT -> GVAR[3]\n"     /* 50（中点） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "MAP script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i == 50,  "MAP 2048/4095 -> 50");
            CHECK(vm()->gvar[1].i == 100, "MAP at inLo -> outLo");
            CHECK(vm()->gvar[2].i == 200, "MAP at inHi -> outHi");
            CHECK(vm()->gvar[3].i == 50,  "MAP midpoint -> 50");
        }
    }

    /* 5) MAP ゼロ除算（inHi==inLo）: RESULT=0 かつ ERR_DIVZERO */
    {
        const char *src =
            "INIT\n"
            "    5, 0, 0, 0, 100 -> MAP\n"
            "    RESULT -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "MAP divzero script compiles");
        if (r==0){
            script_clear_status(ERR_DIVZERO);
            run_init();
            CHECK(vm()->gvar[0].i == 0, "MAP inHi==inLo -> RESULT 0");
            CHECK((script_get_status() & ERR_DIVZERO) != 0, "MAP inHi==inLo sets ERR_DIVZERO");
            script_clear_status(ERR_DIVZERO);
        }
    }

    /* 6) MIN/MAX（可変長）/ ABS（負・INT32_MIN 飽和） */
    {
        const char *src =
            "INIT\n"
            "    5, 2, 9, 1 -> MIN\n"
            "    RESULT -> GVAR[0]\n"     /* 1 */
            "    5, 2, 9, 1 -> MAX\n"
            "    RESULT -> GVAR[1]\n"     /* 9 */
            "    -7 -> ABS\n"
            "    RESULT -> GVAR[2]\n"     /* 7 */
            "    0x80000000 -> ABS\n"
            "    RESULT -> GVAR[3]\n"     /* INT32_MAX=2147483647（飽和） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "MIN/MAX/ABS script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i == 1, "MIN(5,2,9,1) -> 1");
            CHECK(vm()->gvar[1].i == 9, "MAX(5,2,9,1) -> 9");
            CHECK(vm()->gvar[2].i == 7, "ABS(-7) -> 7");
            CHECK(vm()->gvar[3].i == 2147483647, "ABS(INT32_MIN) saturates to INT32_MAX");
        }
    }

    printf(g_fail ? "PHASE10 FAILED (failures=%d)\n" : "PHASE10 PASSED (failures=0)\n", g_fail);
    return g_fail ? 1 : 0;
}
