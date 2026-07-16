/* test_phase13_local.c - フェーズ13（ブロック局所エイリアス def_local）の単体テスト（v0.4.6 §3）
 *
 * def_local で VAR/SVAR/ARG/SARG/リテラルにブロック局所の名前を付ける。読み書き・型・スコープ
 * （別ブロック同名可・大域衝突エラー・二重宣言・位置違反・GVAR不可・ARG書込不可）を検証。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static int32_t t_now(void){ return 0; }   /* v0.4.8: ロード時クロック要求を満たす最小スタブ */
static int compile(const char *s){ return script_load(s, strlen(s)); }
static void run(const char *s){ if (compile(s)==0) script_tick(); }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_now(t_now);   /* v0.4.8: クロック源が無いと script_load が ERR_NO_CLOCK */
    printf("== Phase13 def_local (block-scoped aliases) ==\n");

    /* 1) INIT で VAR/リテラルに局所名（読み書き） */
    {
        const char *src =
            "INIT\n"
            "    def_local(SUM, VAR[0])\n"
            "    def_local(N,   5)\n"
            "    N -> SUM\n"                 /* VAR[0] = 5 */
            "    SUM + 10 -> GVAR[0]\n"      /* 15 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "def_local VAR/int compiles");
        if (r==0){ script_tick();
            CHECK(vm()->var[0].i == 5,   "N -> SUM writes VAR[0]=5");
            CHECK(vm()->gvar[0].i == 15, "SUM + 10 == 15");
        }
    }

    /* 2) SVAR / 文字列リテラルの局所名 */
    {
        const char *src =
            "INIT\n"
            "    def_local(MSG, SVAR[0])\n"
            "    def_local(HI,  \"hello\")\n"
            "    HI -> MSG\n"                /* SVAR[0] = "hello" */
            "    MSG -> SGVAR[0]\n"
            "END\n";
        run(src);
        CHECK(strcmp(vm()->sgvar[0], "hello") == 0, "def_local SVAR/str works");
    }

    /* 3) ハンドラで ARG/SARG に名前（受信を読みやすく） */
    {
        const char *src =
            "def_handler(GO)\n"
            "ON GO\n"
            "    def_local(A, ARG[0])\n"
            "    def_local(B, ARG[1])\n"
            "    A + B -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "def_local ARG in handler compiles");
        if (r==0){
            script_tick();
            script_arg_t a[2]; a[0]=SCRIPT_ARG_INT(5); a[1]=SCRIPT_ARG_INT(7);
            script_post_msg_v("GO", 2, a);
            script_tick();
            CHECK(vm()->gvar[0].i == 12, "A + B (ARG[0]+ARG[1]) == 12");
        }
    }

    /* 4) 別ハンドラ間の同名ローカルは別スロットを指す（許可） */
    {
        const char *src =
            "def_handler(H1)\n"
            "def_handler(H2)\n"
            "ON H1\n"
            "    def_local(X, VAR[0])\n"
            "    100 -> X\n"
            "    X -> GVAR[0]\n"
            "END\n"
            "ON H2\n"
            "    def_local(X, VAR[1])\n"    /* 同名だが別スロット */
            "    200 -> X\n"
            "    X -> GVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "same local name in different handlers compiles");
        if (r==0){
            script_tick();
            script_post_msg("H1", 0); script_tick();
            script_post_msg("H2", 0); script_tick();
            CHECK(vm()->gvar[0].i == 100 && vm()->var[0].i == 100, "H1 X = VAR[0] = 100");
            CHECK(vm()->gvar[1].i == 200 && vm()->var[1].i == 200, "H2 X = VAR[1] = 200");
        }
    }

    /* 5) PORT 本体でも def_local が使える */
    {
        const char *src =
            "def_port(MID, T_INT)\n"
            "PORT MID\n"
            "    def_local(LO, ARG[0])\n"
            "    def_local(HI, ARG[1])\n"
            "    (LO + HI) / 2 -> EXIT\n"
            "END\n"
            "INIT\n"
            "    10, 20 -> MID -> GVAR[0]\n"   /* 15 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "def_local in PORT compiles");
        if (r==0){ script_tick(); CHECK(vm()->gvar[0].i == 15, "MID(10,20) midpoint == 15"); }
    }

    /* ---- エラー系 ---- */

    /* 6) ローカル名が大域エイリアスと衝突 → ERR_SYNTAX */
    {
        const char *src =
            "def_alias(FOO, GVAR[0])\n"
            "ON TIMER\n"
            "    def_local(FOO, VAR[0])\n"    /* 大域 FOO と衝突 */
            "    0 -> FOO\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "local name == global alias -> ERR_SYNTAX");
    }

    /* 7) GVAR を def_local に → ERR_SYNTAX（それは def_alias で） */
    {
        const char *src =
            "ON TIMER\n"
            "    def_local(G, GVAR[0])\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "def_local GVAR -> ERR_SYNTAX");
    }

    /* 8) 文の後の def_local → ERR_SYNTAX（冒頭一括のみ） */
    {
        const char *src =
            "INIT\n"
            "    1 -> GVAR[0]\n"
            "    def_local(X, VAR[0])\n"      /* 文の後 */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "def_local after a statement -> ERR_SYNTAX");
    }

    /* 9) ファイル冒頭の def_local → ERR_SYNTAX（ブロック内のみ） */
    {
        const char *src =
            "def_local(X, VAR[0])\n"
            "INIT\n    0 -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "def_local at file top -> ERR_SYNTAX");
    }

    /* 10) ARG 別名への書き込み → ERR_BAD_POSITION（受信専用） */
    {
        const char *src =
            "def_handler(GO)\n"
            "ON GO\n"
            "    def_local(A, ARG[0])\n"
            "    5 -> A\n"                    /* 受信専用に書く */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_BAD_POSITION, "write to ARG alias -> ERR_BAD_POSITION");
    }

    /* 11) 同一ブロック内の二重宣言 → ERR_SYNTAX */
    {
        const char *src =
            "INIT\n"
            "    def_local(X, VAR[0])\n"
            "    def_local(X, VAR[1])\n"      /* 同名二重 */
            "    0 -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "double-decl same block -> ERR_SYNTAX");
    }

    /* 12) 別ハンドラのローカルは見えない（スコープ外参照）→ ERR_UNKNOWN_NAME */
    {
        const char *src =
            "def_handler(H1)\n"
            "def_handler(H2)\n"
            "ON H1\n"
            "    def_local(X, VAR[0])\n"
            "    1 -> X\n"
            "END\n"
            "ON H2\n"
            "    X -> GVAR[0]\n"              /* H1 の X はここでは見えない */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_UNKNOWN_NAME, "local out of scope -> ERR_UNKNOWN_NAME");
    }

    /* 13) 型不一致（SVAR 別名に int） → ERR_TYPE_MISMATCH */
    {
        const char *src =
            "INIT\n"
            "    def_local(S, SVAR[0])\n"
            "    5 -> S\n"                    /* int を str 別名へ */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_TYPE_MISMATCH, "int -> SVAR alias -> ERR_TYPE_MISMATCH");
    }

    /* 14) 予約語をローカル名に（PORT） → ERR_SYNTAX */
    {
        const char *src =
            "ON TIMER\n"
            "    def_local(PORT, ARG[0])\n"   /* PORT は予約語 */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "reserved word 'PORT' as local name -> ERR_SYNTAX");
    }

    printf("\n%s (fails=%d)\n", g_fail==0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail ? 1 : 0;
}
