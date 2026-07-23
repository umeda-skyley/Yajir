/* test_phase2_compile.c - フェーズ2（トークナイザ＋コンパイラ）の単体テスト
 *
 * 小さなスクリプトをコンパイルし、(a)コンパイル成否と行番号付きエラー、
 * (b)生成バイトコードを vm_exec で直接走らせた結果、を検証する。
 * スケジューラ無しでブロック先頭から実行する。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"
#include "../src/core/compiler.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

/* 記録用OUT */
static int32_t g_seq[64]; static int g_seqn = 0;
static int32_t g_led = 0;
static void out_rec(int argc, const script_value_t *a){ g_seq[g_seqn++] = (argc>0)?a[0].i:0; }
static void calc(int argc, const script_value_t *a){
    int32_t x=(argc>0)?a[0].i:0, y=(argc>1)?a[1].i:0; script_set_result(x+y*2); }
static void led_set(int argc, const script_value_t *a){ g_led=(argc>0)?a[0].i:0; }
static int32_t led_get(void){ return g_led; }
static int32_t now_get(void){ return 0; }

static int find_block(block_kind_t k){
    int i; for(i=0;i<vm()->nblocks;i++) if(vm()->blocks[i].kind==k) return i; return -1; }
static void run_block(int bi, int in_main){
    uint16_t pc = vm()->blocks[bi].bc_start; int budget=CFG_INSTR_BUDGET; int32_t ms=0;
    vm()->sp = 0;
    vm_exec(&pc, &budget, &ms, in_main);
}

static void setup_ports(void){
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_out("OUT", out_rec);
    script_register_out("CALC", calc);
    script_register_inout("LED", led_get, led_set, SCRIPT_T_INT);
    script_register_const("HOT", 30);
    script_register_in("NOW", now_get, SCRIPT_T_INT);
    script_register_handler("BTN");
}

static int compile(const char *s){ return script_load(s, strlen(s)); }

int main(void)
{
    setup_ports();
    printf("== Phase2 tokenizer + compiler ==\n");

    /* 1) 正常コンパイル（def行スキップ・複数ブロック） */
    {
        const char *src =
            "// comment line\n"
            "def_handler(BTN)\n"             /* C側マクロの写し→スキップ */
            "ON 1000\n"
            "    100 -> GVAR[0]\n"
            "    5, 9 -> GVAR[1]\n"          /* 切り捨て：先頭5 */
            "    (GVAR[0] > 50) -> IFYES\n"
            "        7 -> OUT\n"
            "    ELSE\n"
            "        8 -> OUT\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "valid script compiles (r==0)");
        g_seqn = 0;
        if (r == 0) {
            int bi = find_block(BLK_ON_PERIOD);
            run_block(bi, 0);
            CHECK(vm()->gvar[0].i == 100, "100 -> GVAR[0]");
            CHECK(vm()->gvar[1].i == 5,   "5,9 -> GVAR[1] keeps first (==5)");
            CHECK(g_seqn == 1 && g_seq[0] == 7, "IFYES THEN taken -> OUT 7");
            CHECK(vm()->blocks[bi].period == 1000, "ON 1000 period recorded");
        }
    }

    /* 2) 算術・比較・短絡・const・inout読み */
    {
        const char *src =
            "INIT\n"
            "    1 -> LED\n"
            "    LED, HOT -> CALC\n"        /* RESULT = 1 + 30*2 = 61 */
            "    RESULT -> OUT\n"           /* OUT 61 */
            "    (3 + 4 - 2) -> OUT\n"      /* OUT 5 */
            "    (1 AND 0) -> OUT\n"        /* OUT 0 */
            "    (0 OR 9) -> OUT\n"         /* OUT 1 */
            "    (HOT == 30) -> OUT\n"      /* OUT 1 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "arith/logic script compiles");
        g_seqn = 0;
        if (r == 0) {
            run_block(find_block(BLK_INIT), 0);
            CHECK(g_seqn==5 && g_seq[0]==61 && g_seq[1]==5 && g_seq[2]==0 && g_seq[3]==1 && g_seq[4]==1,
                  "RESULT=61, 3+4-2=5, 1 AND 0=0, 0 OR 9=1, HOT==30=1");
        }
    }

    /* 3) WAIT は MAIN で許可 */
    {
        const char *src = "MAIN\n    1000 -> WAIT\n    1 -> LED\nEND\n";
        CHECK(compile(src) == 0, "WAIT compiles inside MAIN");
    }

    /* 4) WAIT は ON で禁止（行番号＋コード, v0.3.7） */
    {
        const char *src = "ON BTN\n    1 -> LED\n    500 -> WAIT\nEND\n";
        int r = compile(src);
        const script_error_t *e = script_last_error();
        CHECK(r != 0, "WAIT in ON is a load error");
        CHECK(e->line == 3 && e->code == ERR_WAIT_IN_ON, "ERR_WAIT_IN_ON @line 3");
    }

    /* 5) 添字範囲外 */
    {
        const char *src = "INIT\n    1 -> GVAR[99]\nEND\n";
        int r = compile(src);
        const script_error_t *e = script_last_error();
        CHECK(r != 0 && e->line == 2 && e->code == ERR_BAD_SLOT_INDEX, "ERR_BAD_SLOT_INDEX @2");
    }

    /* 6) 未定義ポート（tok=名前） */
    {
        const char *src = "INIT\n    1 -> NOPE\nEND\n";
        int r = compile(src);
        const script_error_t *e = script_last_error();
        CHECK(r != 0 && e->line == 2 && e->code == ERR_UNKNOWN_PORT && strcmp(e->tok,"NOPE")==0,
              "ERR_UNKNOWN_PORT tok='NOPE' @2");
    }

    /* 7) ON <条件> エッジは未対応エラー */
    {
        const char *src = "ON HOT > 10\n    1 -> LED\nEND\n";
        CHECK(compile(src) != 0, "ON <condition> edge trigger rejected");
    }

    /* 8) 入力ポートへ送る誤りを検出（NOWは入力） */
    {
        const char *src = "INIT\n    1 -> NOW\nEND\n";
        CHECK(compile(src) != 0, "sending to input port rejected");
    }

    /* 9) 行デリミタ: CR / CRLF / LF どれでも1行として通る（CFG_LINE_DELIM, 既定は両受理） */
    {
        CHECK(compile("INIT\r    100 -> GVAR[0]\r    200 -> GVAR[1]\rEND\r") == 0, "CR line endings compile");
        CHECK(compile("INIT\r\n    100 -> GVAR[0]\r\n    200 -> GVAR[1]\r\nEND\r\n") == 0, "CRLF line endings compile");
        CHECK(compile("INIT\n    100 -> GVAR[0]\nEND\n") == 0, "LF line endings still compile");
        /* CRLF でも行番号が正確（畳んで二重計上しない）: 3行目の未定義ポートを @3 で報告 */
        { const script_error_t *e;
          compile("INIT\r\n    1 -> OUT\r\n    1 -> WIDGET\r\nEND\r\n");
          e = script_last_error();
          CHECK(e->code == ERR_UNKNOWN_PORT && e->line == 3, "CRLF: error line counted once (@3)"); }
    }

    /* 10) CFG_* コンパイル時定数（v0.4.9）: ビルド実値へ int リテラル展開・ポート/alias 非消費 */
    {
        int bi;
        const char *src =
            "INIT\n"
            "    CFG_VAR_COUNT  -> OUT\n"                     /* スロット数 */
            "    CFG_SSTR_LEN   -> OUT\n"                     /* 文字列長 */
            "    (CFG_VAR_COUNT + CFG_SVAR_COUNT) -> OUT\n"   /* 式の中でも定数 */
            "END\n";
        CHECK(compile(src) == 0, "CFG_* constants compile in expressions");
        g_seqn = 0; bi = find_block(BLK_INIT);
        if (bi >= 0) run_block(bi, 0);
        CHECK(g_seqn == 3, "three CFG values emitted");
        CHECK(g_seq[0] == CFG_VAR_COUNT, "CFG_VAR_COUNT == build value");
        CHECK(g_seq[1] == CFG_SSTR_LEN,  "CFG_SSTR_LEN == build value");
        CHECK(g_seq[2] == CFG_VAR_COUNT + CFG_SVAR_COUNT, "CFG_* usable in arithmetic");

        /* CFG_VAR_COUNT -> REPEAT … ITR … END でスロット数ぶん回る（プラットフォーム非依存） */
        CHECK(compile("INIT\n    CFG_VAR_COUNT -> REPEAT\n        ITR -> OUT\n    END\nEND\n") == 0,
              "CFG_VAR_COUNT drives REPEAT count");
        g_seqn = 0; bi = find_block(BLK_INIT);
        if (bi >= 0) run_block(bi, 0);
        CHECK(g_seqn == CFG_VAR_COUNT && g_seq[CFG_VAR_COUNT-1] == CFG_VAR_COUNT,
              "REPEAT ran CFG_VAR_COUNT times (ITR 1..N)");

        /* 読み取り専用: 送り先には立てられない */
        CHECK(compile("INIT\n    1 -> CFG_VAR_COUNT\nEND\n") != 0, "CFG_* cannot be a send target");
        /* 予約名: def_alias で上書き不可 */
        CHECK(compile("def_alias(CFG_VAR_COUNT, 3)\nINIT\n    1 -> OUT\nEND\n") != 0,
              "CFG_* reserved: def_alias cannot shadow it");
        /* 非公開の CFG_ 名は通常の未知名（内部限界は隠す） */
        CHECK(compile("INIT\n    CFG_CODE_SIZE -> OUT\nEND\n") != 0,
              "unexposed CFG_ name is an unknown name (internals hidden)");
    }

    /* 11) スロット添字のコンパイル時定数式（v0.4.11）: CFG_*、別名、数値の四則で固定添字を書ける */
    {
        int bi;
        /* GVAR[CFG_GVAR_COUNT-1] = 最上位スロット（def_import ライブラリの「高位から降順」規約を移植可能に） */
        CHECK(compile("INIT\n    77 -> GVAR[CFG_GVAR_COUNT-1]\n    GVAR[CFG_GVAR_COUNT-1] -> OUT\nEND\n") == 0,
              "GVAR[CFG_GVAR_COUNT-1] compiles (const-expr index)");
        g_seqn = 0; bi = find_block(BLK_INIT); if (bi >= 0) run_block(bi, 0);
        CHECK(g_seqn == 1 && g_seq[0] == 77, "const-expr index resolves to the top slot");

        /* 別名の整数定数も添字に使える（従来は T_IDENT ゆえ弾かれていた） */
        CHECK(compile("def_alias(IDX, 2)\nINIT\n    9 -> VAR[IDX]\n    VAR[IDX*1] -> OUT\nEND\n") == 0,
              "alias int constant usable as index (VAR[IDX])");
        g_seqn = 0; bi = find_block(BLK_INIT); if (bi >= 0) run_block(bi, 0);
        CHECK(g_seqn == 1 && g_seq[0] == 9, "alias index resolves");

        /* 括弧・乗除・剰余 */
        CHECK(compile("INIT\n    5 -> GVAR[(CFG_GVAR_COUNT-2)*1]\nEND\n") == 0, "parens & mul in index");
        /* 範囲外は従来どおり ERR_BAD_SLOT_INDEX（定数式でも静的に弾く） */
        CHECK(compile("INIT\n    1 -> GVAR[CFG_GVAR_COUNT]\nEND\n") != 0, "const-expr out of range rejected");
        /* def_alias 右辺のスロット添字にも効く＝ライブラリの高位スロット規約が書ける */
        CHECK(compile("def_alias(LIB_TOP, GVAR[CFG_GVAR_COUNT-1])\nINIT\n    5 -> LIB_TOP\nEND\n") == 0,
              "def_alias(..., GVAR[CFG_GVAR_COUNT-1]) compiles");
        /* 添字にスロット別名（非整数）は使えない＝定数でないので拒否 */
        CHECK(compile("def_alias(P, GVAR[0])\nINIT\n    1 -> VAR[P]\nEND\n") != 0,
              "non-int name in index is rejected");
        /* 0除算の定数式は添字エラー */
        CHECK(compile("INIT\n    1 -> GVAR[1/0]\nEND\n") != 0, "divide-by-zero in const index rejected");
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE2 FAILED" : "PHASE2 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
