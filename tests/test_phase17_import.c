/* test_phase17_import.c - フェーズ17（def_import ライブラリ取り込み）の単体テスト（v0.4.10 §13）
 *
 * def_import("NAME") は「構文はコア／実体はホスト注入」。ここでは注入関数をテスト側で用意して、
 *   (1) 取り込みが実際にコンパイルされること（ハンドラ/別名がアプリから見える＝大域エクスポート）
 *   (2) 未サポート/名前なしが**黙って無視されず**ロードエラーになること
 *   (3) 禁止事項（ライブラリ内 INIT/MAIN・入れ子 import・最上部以外）が弾かれること
 *   (4) エラーがどのソースの行かを src_name で切り分けられること
 *   (5) 二重 import が冪等で、複数ライブラリの取り込みができること
 * を検証する。ライブラリ本文はテスト内の const char[]（実機の Flash 常駐表と同じ形）。
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
/* n tick 回す（1 tick = 10ms 進める）。INIT は最初の tick で走り、post されたハンドラは次の tick。 */
static void run_ticks(int n){ int i; for (i=0;i<n;i++){ script_tick(); g_now += 10; } }

/* STDOUT シンク：出力を貯めて内容を検査する */
static char g_out[256];
static int  g_outlen = 0;
static void cap_puts(const char *s){
    int n = (int)strlen(s);
    if (g_outlen + n < (int)sizeof(g_out)) { memcpy(g_out + g_outlen, s, n); g_outlen += n; g_out[g_outlen] = '\0'; }
}

/* ---- ライブラリ表（実機 yajir_libs.h と同じ形）---- */
static const char LIB_GOOD[] =
    "def_alias(LIB_MS, GVAR[7])\n"        /* エクスポート（アプリから見える） */
    "def_handler(LIB_START)\n"
    "def_port(LIB_DOUBLE, T_INT)\n"
    "PORT LIB_DOUBLE\n"
    "    ARG[0] * 2 -> EXIT\n"
    "END\n"
    "ON LIB_START\n"
    "    1 -> GVAR[0]\n"
    "    \"LIB_DONE\" -> INVOKER\n"       /* weak link: アプリが受けていれば呼ばれる */
    "END\n";
static const char LIB_OTHER[] =
    "def_alias(OTHER_VAL, 7)\n";
static const char LIB_WITH_INIT[] =       /* 禁止: ライブラリに INIT */
    "def_handler(X1)\n"
    "INIT\n"
    "    1 -> GVAR[1]\n"
    "END\n";
static const char LIB_WITH_MAIN[] =       /* 禁止: ライブラリに MAIN */
    "MAIN\n"
    "    1 -> GVAR[1]\n"
    "END\n";
static const char LIB_NESTED[] =          /* 禁止: ライブラリ内の def_import */
    "def_import(\"other\")\n";
static const char LIB_BROKEN[] =          /* 3行目で壊れている（src_name/line の確認用） */
    "def_handler(B1)\n"
    "ON B1\n"
    "    1 -> NOSUCHPORT\n"
    "END\n";

static int g_import_calls = 0;
static int test_import(const char *name, const char **src, uint32_t *len)
{
    const char *s = NULL;
    g_import_calls++;
    if      (!strcmp(name, "good"))      s = LIB_GOOD;
    else if (!strcmp(name, "other"))     s = LIB_OTHER;
    else if (!strcmp(name, "with_init")) s = LIB_WITH_INIT;
    else if (!strcmp(name, "with_main")) s = LIB_WITH_MAIN;
    else if (!strcmp(name, "nested"))    s = LIB_NESTED;
    else if (!strcmp(name, "broken"))    s = LIB_BROKEN;
    if (!s) return -1;                    /* 名前なし → ERR_IMPORT_NOT_FOUND */
    *src = s; *len = (uint32_t)strlen(s);
    return 0;
}

/* 毎ケース同じ土台（クロック＋STDOUT＋import）で作り直す */
static void setup(char *arena, size_t sz, int with_import)
{
    script_init(arena, sz);
    script_register_now(now_get);
    script_register_stdout(cap_puts);
    if (with_import) script_register_import(test_import);
    g_outlen = 0; g_out[0] = '\0';
}

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    printf("== Phase17 def_import (library injection) ==\n");

    /* 1) 取り込みが実際にコンパイルされ、ライブラリの別名/ハンドラ/ポートがアプリから使える。
     *    ＝ def_alias は大域エクスポート（ファイル局所にしない）という設計判断の検証。 */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        g_import_calls = 0;
        r = compile("def_import(\"good\")\n"
                    "def_handler(LIB_DONE)\n"
                    "INIT\n"
                    "    250 -> LIB_MS\n"            /* ライブラリの別名に書ける（GVAR[7]） */
                    "    21 -> LIB_DOUBLE -> GVAR[2]\n" /* ライブラリのスクリプトポートを呼べる */
                    "    none -> LIB_START AFTER 10\n"   /* ライブラリのハンドラを蹴れる。INIT からの即post は橋で捨てられるので AFTER で張る（§10） */
                    "END\n"
                    "ON LIB_DONE\n"
                    "    \"done\" -> STDOUT\n"
                    "END\n");
        CHECK(r == 0, "script with def_import compiles");
        CHECK(g_import_calls == 1, "host import function is called once");
        if (r == 0) {
            run_ticks(1);   /* INIT が走る */
            CHECK(vm()->gvar[7].i == 250, "library alias (LIB_MS -> GVAR[7]) is visible to the importer");
            CHECK(vm()->gvar[2].i == 42,  "library script port (LIB_DOUBLE) is callable from the importer");
            run_ticks(4);   /* 遅延post満期 → LIB_START 本体 → INVOKER → ON LIB_DONE */
            CHECK(vm()->gvar[0].i == 1, "library handler body runs");
            CHECK(strcmp(g_out, "done\r\n") == 0, "INVOKER weak link reaches the importer's ON LIB_DONE");
        }
    }

    /* 2) weak link は片側だけでも成立する: アプリが ON LIB_DONE を持たなくても落ちない */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "INIT\n"
                    "    none -> LIB_START AFTER 10\n"   /* INIT からの即post は橋で捨てられる＝AFTER で張る（§10） */
                    "END\n");
        CHECK(r == 0, "importer without the callback handler still compiles");
        if (r == 0) { run_ticks(5); CHECK(vm()->gvar[0].i == 1, "unclaimed INVOKER name is ignored (no crash)"); }
    }

    /* 3) 未サポートのプラットフォーム: 注入関数が無ければ ERR_NO_IMPORT（黙って無視しない）。
     *    ※ 未知の def_* は skip_def_line で読み飛ばされる仕様なので、ここを明示的に潰すのが肝。 */
    {
        int r;
        setup(arena, sizeof(arena), 0);   /* import 関数を登録しない */
        r = compile("def_import(\"good\")\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_NO_IMPORT, "no import function -> ERR_NO_IMPORT (not silently skipped)");
        CHECK(strcmp(script_last_error()->tok, "good") == 0, "error carries the library name");
    }

    /* 4) 名前が無い: ERR_IMPORT_NOT_FOUND */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"nope\")\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_IMPORT_NOT_FOUND, "unknown library -> ERR_IMPORT_NOT_FOUND");
        CHECK(strcmp(script_last_error()->tok, "nope") == 0, "error carries the missing library name");
    }

    /* 5) ライブラリには INIT / MAIN を書けない（§13） */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"with_init\")\nINIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "INIT inside a library is rejected");
        CHECK(strcmp(script_last_error()->src_name, "with_init") == 0, "…and is blamed on the library");

        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"with_main\")\nINIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "MAIN inside a library is rejected");
    }

    /* 6) 入れ子 import は禁止（退避を1段に保つための制約） */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"nested\")\nINIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "nested def_import inside a library is rejected");
        CHECK(strcmp(script_last_error()->src_name, "nested") == 0, "…and is blamed on the library");
    }

    /* 7) def_import はファイル最上部のみ（他の宣言/ブロックより前）。
     *    この規約が「ライブラリの別名は見えるが、ライブラリからは本体が見えない」を時系列で作る。 */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_alias(APP_X, GVAR[1])\n"
                    "def_import(\"good\")\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "def_import after another declaration is rejected (top of file only)");
    }

    /* 8) 複数ライブラリの取り込み（連続する def_import は通る）＋二重 import は冪等 no-op */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        g_import_calls = 0;
        r = compile("def_import(\"good\")\n"
                    "def_import(\"other\")\n"
                    "def_import(\"good\")\n"        /* 2回目は取得すら呼ばれない */
                    "INIT\n    OTHER_VAL -> GVAR[3]\nEND\n");
        CHECK(r == 0, "multiple def_import lines compile");
        CHECK(g_import_calls == 2, "duplicate import is an idempotent no-op (host not asked twice)");
        if (r == 0) { run_ticks(1); CHECK(vm()->gvar[3].i == 7, "alias from the second library is visible too"); }
    }

    /* 9) 名前衝突は既存の重複検出がそのまま効く（追加機構なし）。ライブラリが先に登録されるので
     *    エラーは本体側の行に出る＝改名すべき側が指される。 */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "def_alias(LIB_MS, GVAR[1])\n"   /* ライブラリと同名 */
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "name collision with a library alias is rejected");
        CHECK(script_last_error()->src_name[0] == '\0', "…and is blamed on the main script (not the library)");
    }

    /* 10) ライブラリ内のエラーは src_name＋ライブラリ内の行番号で報告される（行番号の二重帳簿対策） */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"broken\")\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_UNKNOWN_PORT, "error inside a library propagates (unknown port)");
        CHECK(strcmp(script_last_error()->src_name, "broken") == 0, "src_name identifies the failing library");
        CHECK(script_last_error()->line == 3, "line number is the library's own line, not the importer's");
    }

    /* 11) 取り込み後も本体スクリプトのエラーは src_name 空（＝本体）に戻る */
    {
        int r;
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "INIT\n    1 -> NOSUCHPORT\nEND\n");
        CHECK(r == ERR_UNKNOWN_PORT, "error after the import is still reported");
        CHECK(script_last_error()->src_name[0] == '\0', "src_name is empty again for the main script");
    }

    /* 12) スクリプト内ポートの二重定義は本体⇄ライブラリをまたいでも ERR_DUP_DEF（v0.4.11）。
     *     LIB_GOOD は def_port(LIB_DOUBLE)+PORT LIB_DOUBLE を持つ。アプリが同名を再定義したら弾く。 */
    {
        int r;
        /* (a) アプリが def_port を再宣言 */
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "def_port(LIB_DOUBLE, T_INT)\n"
                    "PORT LIB_DOUBLE\n    9 -> EXIT\nEND\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_DUP_DEF, "re-declaring a library's def_port -> ERR_DUP_DEF");
        CHECK(script_last_error()->src_name[0] == '\0', "…blamed on the main script");
        CHECK(strcmp(script_last_error()->tok, "LIB_DOUBLE") == 0, "…names the clashing port");

        /* (b) アプリが PORT 本体だけ書く（def_port なし）＝本体二重定義 */
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "PORT LIB_DOUBLE\n    9 -> EXIT\nEND\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_DUP_DEF, "re-defining a library's PORT body -> ERR_DUP_DEF");

        /* (c) アプリが def_handler で同名を奪おうとする（種別衝突） */
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "def_handler(LIB_DOUBLE)\n"
                    "INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_DUP_DEF, "def_handler colliding with a library port -> ERR_DUP_DEF");

        /* (d) クリーンな利用は通る（回帰防止） */
        setup(arena, sizeof(arena), 1);
        r = compile("def_import(\"good\")\n"
                    "INIT\n    21 -> LIB_DOUBLE -> GVAR[0]\nEND\n");
        CHECK(r == 0 && (run_ticks(1), vm()->gvar[0].i == 42), "clean use of the library port still works");
    }

    printf(g_fail ? "PHASE17 FAILED (failures=%d)\n" : "PHASE17 PASSED (failures=%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
