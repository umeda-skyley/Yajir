/* test_phase1_vm.c - フェーズ1（値・ポート・VMコア）の単体テスト
 *
 * コンパイラはまだ無いので、バイトコードを手組みして vm_exec を直接叩き、
 * スタック/スロット/RESULT/比較/算術/ポート呼び/STR/タイマarm を検証する。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

/* ---- 簡易アサート ---- */
static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok   : %s\n", msg); } \
    else      { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

/* ---- バイトコード手組みヘルパ ---- */
static int CP;
static void reset_code(void) { CP = 0; vm()->sp = 0; }
static void e8(int b)    { vm()->code[CP++] = (uint8_t)b; }
static void ei32(int32_t v){ e8(v); e8(v>>8); e8(v>>16); e8(v>>24); }
static void eu16(int v)  { e8(v); e8(v>>8); }
static void patch16(int at, int v){ vm()->code[at]=(uint8_t)v; vm()->code[at+1]=(uint8_t)(v>>8); }

static exec_status_t run(void)
{
    uint16_t pc = 0; int budget = CFG_INSTR_BUDGET; int32_t ms = 0;
    return vm_exec(&pc, &budget, &ms, /*in_main=*/true);
}

/* ---- テスト用モックポート ---- */
static int  g_out_argc = -1;
static int32_t g_out_a0 = 0, g_out_a1 = 0;
static int  g_side_called = 0;

/* PK_OUT: 引数を記録し、RESULT = a0 + a1 */
static void out_rec(int argc, const script_value_t *argv)
{
    g_out_argc = argc;
    g_out_a0 = (argc > 0) ? argv[0].i : 0;
    g_out_a1 = (argc > 1) ? argv[1].i : 0;
    script_set_result(g_out_a0 + g_out_a1);
}
/* PK_OUT: 呼ばれたら副作用フラグを立てる（短絡確認用） */
static void out_side(int argc, const script_value_t *argv)
{
    (void)argc; (void)argv; g_side_called = 1;
}
/* PK_IN: 常に 42 */
static int32_t in_42(void) { return 42; }
/* PK_IN: NOW=1000固定 */
static int32_t in_now(void) { return 1000; }

int main(void)
{
    static char arena[sizeof(script_vm_t) + 64];
    int p_out, p_side, p_in, p_now;

    script_init(arena, sizeof(arena));
    script_register_out("OUT",  out_rec);
    script_register_out("SIDE", out_side);
    script_register_in ("IN42", in_42, SCRIPT_T_INT);
    script_register_in ("NOW",  in_now, SCRIPT_T_INT);
    p_out  = vm_find_port("OUT");
    p_side = vm_find_port("SIDE");
    p_in   = vm_find_port("IN42");
    p_now  = vm_find_port("NOW");

    printf("== Phase1 VM core ==\n");

    /* 1) 算術 + GVAR格納: (3 + 4) -> GVAR[0] */
    reset_code();
    e8(OP_PUSH_INT); ei32(3);
    e8(OP_PUSH_INT); ei32(4);
    e8(OP_ADD);
    e8(OP_STORE_GVAR); e8(0); e8(1);
    e8(OP_HALT);
    run();
    CHECK(vm()->gvar[0].i == 7, "3+4 -> GVAR[0] == 7");

    /* 2) 比較: (5 > 3) -> GVAR[1] == 1, (2 >= 9) -> GVAR[2] == 0 */
    reset_code();
    e8(OP_PUSH_INT); ei32(5); e8(OP_PUSH_INT); ei32(3); e8(OP_GT);
    e8(OP_STORE_GVAR); e8(1); e8(1);
    e8(OP_PUSH_INT); ei32(2); e8(OP_PUSH_INT); ei32(9); e8(OP_GE);
    e8(OP_STORE_GVAR); e8(2); e8(1);
    e8(OP_HALT);
    run();
    CHECK(vm()->gvar[1].i == 1, "5>3 == 1");
    CHECK(vm()->gvar[2].i == 0, "2>=9 == 0");

    /* 3) STORE 切り捨て: (8, 9) -> GVAR[3] は先頭8を格納 */
    reset_code();
    e8(OP_PUSH_INT); ei32(8); e8(OP_PUSH_INT); ei32(9);
    e8(OP_STORE_GVAR); e8(3); e8(2);
    e8(OP_HALT);
    run();
    CHECK(vm()->gvar[3].i == 8, "(8,9)->GVAR[3] keeps first (==8)");
    CHECK(vm()->sp == 0, "stack balanced after store");

    /* 4) CALL_OUT 2引数 + RESULT: (10,20)->OUT, RESULT==30 */
    reset_code();
    e8(OP_PUSH_INT); ei32(10); e8(OP_PUSH_INT); ei32(20);
    e8(OP_CALL_OUT); e8(p_out); e8(2);
    e8(OP_LOAD_RESULT); e8(OP_STORE_GVAR); e8(4); e8(1);
    e8(OP_HALT);
    run();
    CHECK(g_out_argc == 2 && g_out_a0 == 10 && g_out_a1 == 20, "OUT got (10,20)");
    CHECK(vm()->gvar[4].i == 30, "RESULT == 30 after function port");

    /* 5) LOAD_PORT 入力getter: IN42 -> GVAR[5] == 42 */
    reset_code();
    e8(OP_LOAD_PORT); e8(p_in);
    e8(OP_STORE_GVAR); e8(5); e8(1);
    e8(OP_HALT);
    run();
    CHECK(vm()->gvar[5].i == 42, "LOAD_PORT IN42 == 42");

    /* 6) NOT: NOT 0 == 1 */
    reset_code();
    e8(OP_PUSH_INT); ei32(0); e8(OP_NOT);
    e8(OP_STORE_GVAR); e8(6); e8(1); e8(OP_HALT);
    run();
    CHECK(vm()->gvar[6].i == 1, "NOT 0 == 1");

    /* 7) PUSH_STR + vm_str 解決 */
    {
        const char *s = "HI";
        int off = vm()->strpool_len;
        memcpy(&vm()->strpool[off], s, strlen(s) + 1);
        vm()->strpool_len += (int)strlen(s) + 1;
        reset_code();
        e8(OP_PUSH_STR); eu16(off);
        e8(OP_STORE_GVAR); e8(7); e8(1); e8(OP_HALT);
        run();
        CHECK(vm()->gvar[7].tag == SV_STR, "PUSH_STR has SV_STR tag");
        CHECK(strcmp(vm_str(vm()->gvar[7].i), "HI") == 0, "vm_str resolves \"HI\"");
    }

    /* 8) 短絡AND機構: 0 AND SIDE() は SIDE を呼ばない（JZで飛ぶ） */
    g_side_called = 0;
    reset_code();
    {
        int jz1, jz2, jmp1, lfalse, lend;
        e8(OP_PUSH_INT); ei32(0);
        e8(OP_JZ); jz1 = CP; eu16(0);        /* false なら Lfalse へ */
        e8(OP_PUSH_INT); ei32(0);            /* 本来RHS評価。ここでSIDEを呼ぶ */
        e8(OP_CALL_OUT); e8(p_side); e8(0);
        e8(OP_PUSH_INT); ei32(1);
        e8(OP_JZ); jz2 = CP; eu16(0);
        e8(OP_PUSH_INT); ei32(1);
        e8(OP_JMP); jmp1 = CP; eu16(0);
        lfalse = CP; e8(OP_PUSH_INT); ei32(0);
        lend = CP;
        e8(OP_STORE_GVAR); e8(8); e8(1); e8(OP_HALT);
        patch16(jz1, lfalse); patch16(jz2, lfalse); patch16(jmp1, lend);
    }
    run();
    CHECK(g_side_called == 0, "short-circuit: 0 AND .. skips RHS (SIDE not called)");
    CHECK(vm()->gvar[8].i == 0, "short-circuit AND result == 0");

    /* 9) ARM_TIMER: 300 -> TIMER がスロットを1本使う（NOW=1000なので fire=1300） */
    (void)p_now;
    vm()->init_done = 1;   /* RUNフェーズを明示（v0.3.4: INIT中は t0起点の相対armになる） */
    reset_code();
    e8(OP_PUSH_INT); ei32(300); e8(OP_ARM_TIMER); e8(OP_HALT);
    run();
    CHECK(vm()->timers[0].active && vm()->timers[0].fire_time == 1300,
          "ARM_TIMER sets slot fire_time = NOW+300");

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE1 FAILED" : "PHASE1 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
