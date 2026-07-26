/* test_phase19_defauto.c - フェーズ19（def_auto＝局所スロットの自動割り当て）の単体テスト（v0.4.12 §4）
 *
 * `def_auto(NAME[, T_INT|T_STR])` は def_local の「番号を人間が管理しない」版。
 *   - 名前は宣言必須（＝打ち間違いは従来どおり未知名エラー。暗黙宣言は採らない）
 *   - 番号だけ自動（高位から降順に確保）＝振り直しの手間と二重割り当ての危険が消える
 *   - ブロック局所（def_local と同じ high-water-mark 寿命）＝兄弟ブロックは枠を再利用
 *   - 枯渇はその宣言行で ERR_NO_FREE_SLOT（tok に枯れた資源名＝上げるべき CFG_* が分かる）
 * ここでは読み書き・型・スコープ・枯渇・位置違反を検証する。
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

static char g_out[256]; static int g_outlen = 0;
static void cap_puts(const char *s){
    int n = (int)strlen(s);
    if (g_outlen + n < (int)sizeof(g_out)) { memcpy(g_out + g_outlen, s, n); g_outlen += n; g_out[g_outlen] = '\0'; }
}

static void setup(char *arena, size_t sz)
{
    script_init(arena, sz);
    script_register_now(now_get);
    script_register_stdout(cap_puts);
    g_outlen = 0; g_out[0] = '\0'; g_now = 0;
}

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    printf("== Phase19 def_auto (automatic local slot allocation) ==\n");

    /* 1) 基本: 名前を宣言するだけで使える（読み書き）。番号は書かない。 */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n"
                      "    def_auto(COUNT)\n"
                      "    5 -> COUNT\n"
                      "    COUNT * 2 -> GVAR[0]\n"
                      "END\n") == 0, "def_auto(NAME) compiles and is read/write");
        run_ticks(1);
        CHECK(vm()->gvar[0].i == 10, "auto slot holds the value (5*2==10)");
    }

    /* 2) 高位から降順に確保する（生の VAR[k] は低位から使う慣習と正面衝突しにくい） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n"
                      "    def_auto(A)\n"
                      "    def_auto(B)\n"
                      "    11 -> A\n"
                      "    22 -> B\n"
                      "END\n") == 0, "two def_auto compile");
        run_ticks(1);
        CHECK(vm()->var[CFG_VAR_COUNT-1].i == 11, "first auto slot is the top VAR");
        CHECK(vm()->var[CFG_VAR_COUNT-2].i == 22, "second goes downward");
        CHECK(vm()->var[0].i == 0, "low slots left untouched (for raw VAR[k] use)");
    }

    /* 3) 型指定: T_INT→VAR / T_STR→SVAR（省略時 T_INT） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n"
                      "    def_auto(N, T_INT)\n"
                      "    def_auto(S, T_STR)\n"
                      "    42 -> N\n"
                      "    \"hi\" -> S\n"
                      "    N -> GVAR[0]\n"
                      "    S -> STDOUT\n"
                      "END\n") == 0, "T_INT / T_STR forms compile");
        run_ticks(1);
        CHECK(vm()->gvar[0].i == 42, "T_INT auto slot works");
        CHECK(strcmp(g_out, "hi\r\n") == 0, "T_STR auto slot holds a string");
        CHECK(strcmp(vm()->svar[CFG_SVAR_COUNT-1], "hi") == 0, "string auto slot is the top SVAR");
    }

    /* 4) 型は守られる（int スロットへ文字列は ERR_TYPE_MISMATCH） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    def_auto(N, T_INT)\n    \"x\" -> N\nEND\n") == ERR_TYPE_MISMATCH,
              "assigning a string to a T_INT auto slot is rejected");
    }

    /* 5) ブロック局所: 別ブロックで同名を使い回せる＆枠も再利用される */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_handler(H)\n"
                      "INIT\n"
                      "    def_auto(TMP)\n"
                      "    7 -> TMP\n"
                      "    TMP -> GVAR[0]\n"
                      "    none -> H AFTER 10\n"
                      "END\n"
                      "ON H\n"
                      "    def_auto(TMP)\n"            /* 同名を別ブロックで再宣言できる */
                      "    9 -> TMP\n"
                      "    TMP -> GVAR[1]\n"
                      "END\n") == 0, "same name reused in another block");
        run_ticks(5);
        CHECK(vm()->gvar[0].i == 7 && vm()->gvar[1].i == 9, "both blocks got their own auto slot");
    }

    /* 6) def_local と混在でき、名前の衝突は従来どおり弾かれる */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    def_local(X, VAR[0])\n    def_auto(Y)\n    1 -> X\n    2 -> Y\nEND\n") == 0,
              "def_local and def_auto coexist");
        CHECK(compile("INIT\n    def_auto(Z)\n    def_auto(Z)\nEND\n") != 0,
              "duplicate name is rejected");
        CHECK(compile("def_alias(G, GVAR[0])\nINIT\n    def_auto(G)\nEND\n") != 0,
              "collision with a global alias is rejected");
        CHECK(compile("INIT\n    def_auto(VAR)\nEND\n") != 0, "reserved slot name is rejected");
    }

    /* 7) ★枯渇はその宣言行で ERR_NO_FREE_SLOT（tok に枯れた資源名） */
    {
        char src[1024]; int i, n;
        setup(arena, sizeof(arena));
        n = sprintf(src, "INIT\n");
        for (i = 0; i < CFG_VAR_COUNT + 1; i++) n += sprintf(src + n, "    def_auto(V%d)\n", i);
        sprintf(src + n, "END\n");
        CHECK(compile(src) == ERR_NO_FREE_SLOT, "running out of VAR slots -> ERR_NO_FREE_SLOT");
        CHECK(strcmp(script_last_error()->tok, "VAR") == 0, "tok names the exhausted resource (VAR)");
        CHECK(script_last_error()->line == CFG_VAR_COUNT + 2, "error points at the offending declaration line");

        setup(arena, sizeof(arena));
        n = sprintf(src, "INIT\n");
        for (i = 0; i < CFG_SVAR_COUNT + 1; i++) n += sprintf(src + n, "    def_auto(S%d, T_STR)\n", i);
        sprintf(src + n, "END\n");
        CHECK(compile(src) == ERR_NO_FREE_SLOT, "running out of SVAR slots -> ERR_NO_FREE_SLOT");
        CHECK(strcmp(script_last_error()->tok, "SVAR") == 0, "tok names the exhausted resource (SVAR)");
    }

    /* 8) ちょうど使い切るのは通る（境界） */
    {
        char src[1024]; int i, n;
        setup(arena, sizeof(arena));
        n = sprintf(src, "INIT\n");
        for (i = 0; i < CFG_VAR_COUNT; i++) n += sprintf(src + n, "    def_auto(V%d)\n", i);
        sprintf(src + n, "END\n");
        CHECK(compile(src) == 0, "using exactly CFG_VAR_COUNT slots is fine (boundary)");
    }

    /* 9) 位置違反: 宣言はブロック冒頭一括のみ（文の後・ファイル冒頭は不可） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("INIT\n    1 -> GVAR[0]\n    def_auto(T)\nEND\n") != 0,
              "def_auto after a statement is rejected");
        CHECK(compile("def_auto(T)\nINIT\n    1 -> GVAR[0]\nEND\n") != 0,
              "def_auto at file top-level is rejected (block only)");
    }

    /* 10) PORT 本体でも使える（VAR/SVAR は呼び出しで private＝真のローカル） */
    {
        setup(arena, sizeof(arena));
        CHECK(compile("def_port(SUM, T_INT)\n"
                      "PORT SUM\n"
                      "    def_auto(ACC)\n"
                      "    ARG[0] + ARG[1] -> ACC\n"
                      "    ACC -> EXIT\n"
                      "END\n"
                      "INIT\n"
                      "    def_auto(KEEP)\n"
                      "    100 -> KEEP\n"
                      "    3, 4 -> SUM -> GVAR[0]\n"
                      "    KEEP -> GVAR[1]\n"          /* 呼び出しで壊れないこと */
                      "END\n") == 0, "def_auto inside a PORT body compiles");
        run_ticks(1);
        CHECK(vm()->gvar[0].i == 7, "PORT's own auto slot works (3+4)");
        CHECK(vm()->gvar[1].i == 100, "caller's auto slot survives the call (VAR is private)");
    }

    /* 11) 別名表満杯も同じ専用コードになった（従来 ERR_SYNTAX・v0.4.12 で格上げ） */
    {
        char src[4096]; int i, n;
        setup(arena, sizeof(arena));
        n = sprintf(src, "");
        for (i = 0; i < CFG_MAX_ALIAS + 1; i++) n += sprintf(src + n, "def_alias(A%d, %d)\n", i, i);
        sprintf(src + n, "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(compile(src) == ERR_NO_FREE_SLOT, "alias table full -> ERR_NO_FREE_SLOT (was ERR_SYNTAX)");
        CHECK(strcmp(script_last_error()->tok, "ALIAS") == 0, "tok names the alias table");
    }

    printf(g_fail ? "PHASE19 FAILED (failures=%d)\n" : "PHASE19 PASSED (failures=%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
