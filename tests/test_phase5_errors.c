/* test_phase5_errors.c - フェーズ5/構造化ロードエラー（v0.3.7 §11）
 *
 * script_load が script_err_t コードを返し、script_last_error() が
 * {line, code, aux, tok} を返すことを検証。文字列化はコアでしない（(B)方針）。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static int32_t now_get(void){ return 0; }
static void out_rec(int a, const script_value_t *v){ (void)a;(void)v; }

static void setup(void){
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in ("NOW", now_get, SCRIPT_T_INT);
    script_register_in ("READER",  now_get, SCRIPT_T_STR);  /* str産出の入力ポート（型チェック用, v0.3.8） */
    script_register_in ("READER2", now_get, SCRIPT_T_STR);  /* 2本目（単一SRESULT衝突の検査用） */
    script_register_out("OUT", out_rec);
    script_register_inout("LED", now_get, out_rec, SCRIPT_T_INT);
    script_register_const("HOT", 30);
    script_register_handler("BTN");
}

/* コンパイルして構造化エラーを返す（成功時 code=ERR_NONE, line=0） */
static const script_error_t* comp(const char *s){
    script_load(s, strlen(s));
    return script_last_error();
}

int main(void)
{
    const script_error_t *e;
    setup();
    printf("== Phase5 structured load errors (v0.3.7) ==\n");

    /* 戻り値とコードの整合：成功は 0 / 失敗は非0 = code */
    {
        const char *ok = "INIT\n    1 -> OUT\nEND\n";
        CHECK(script_load(ok, strlen(ok)) == 0, "valid load returns 0");
        CHECK(script_last_error()->code == ERR_NONE, "  and code == ERR_NONE");
    }

    /* WAIT は INIT で解禁（v0.3.4） */
    CHECK(comp("INIT\n    100 -> WAIT\nEND\n")->code == ERR_NONE, "WAIT in INIT is OK");

    /* WAIT in ON → ERR_WAIT_IN_ON @3 */
    e = comp("ON BTN\n    1 -> OUT\n    5 -> WAIT\nEND\n");
    CHECK(e->code == ERR_WAIT_IN_ON && e->line == 3, "ERR_WAIT_IN_ON @line 3");

    /* 添字範囲外 → ERR_BAD_SLOT_INDEX（tok=スロット名） */
    e = comp("INIT\n    1 -> GVAR[100]\nEND\n");
    CHECK(e->code == ERR_BAD_SLOT_INDEX && e->line == 2 && strcmp(e->tok,"GVAR")==0, "ERR_BAD_SLOT_INDEX tok=GVAR @2");
    e = comp("ON BTN\n    ARG[9] -> OUT\nEND\n");
    CHECK(e->code == ERR_BAD_SLOT_INDEX && e->line == 2, "ARG[9] -> ERR_BAD_SLOT_INDEX @2");

    /* 未定義の送り先 → ERR_UNKNOWN_PORT（tok=名前） */
    e = comp("INIT\n    1 -> WIDGET\nEND\n");
    CHECK(e->code == ERR_UNKNOWN_PORT && e->line == 2 && strcmp(e->tok,"WIDGET")==0, "ERR_UNKNOWN_PORT tok=WIDGET @2");

    /* 未定義の名前（式中） → ERR_UNKNOWN_NAME */
    e = comp("INIT\n    NWO -> OUT\nEND\n");
    CHECK(e->code == ERR_UNKNOWN_NAME && e->line == 2 && strcmp(e->tok,"NWO")==0, "ERR_UNKNOWN_NAME tok=NWO @2");

    /* 型違い代入 → ERR_TYPE_MISMATCH */
    e = comp("INIT\n    \"x\" -> GVAR[0]\nEND\n");
    CHECK(e->code == ERR_TYPE_MISMATCH && e->line == 2, "string->int slot = ERR_TYPE_MISMATCH @2");
    e = comp("INIT\n    1 -> SVAR[0]\nEND\n");
    CHECK(e->code == ERR_TYPE_MISMATCH && e->line == 2, "int->string slot = ERR_TYPE_MISMATCH @2");

    /* read-only スロットへの送信＝役割違反 → ERR_SYNTAX（スロットは向き軸外） */
    CHECK(comp("INIT\n    1 -> RESULT\nEND\n")->code == ERR_SYNTAX, "send to RESULT = ERR_SYNTAX");
    CHECK(comp("ON BTN\n    1 -> SARG[0]\nEND\n")->code == ERR_SYNTAX, "send to SARG = ERR_SYNTAX");

    /* ポート向き違反 → ERR_BAD_POSITION（v0.3.8 属性システム） */
    CHECK(comp("INIT\n    1 -> NOW\nEND\n")->code == ERR_BAD_POSITION, "send to in port = ERR_BAD_POSITION");
    CHECK(comp("INIT\n    1 -> HOT\nEND\n")->code == ERR_BAD_POSITION, "send to const = ERR_BAD_POSITION");
    CHECK(comp("INIT\n    OUT -> GVAR[0]\nEND\n")->code == ERR_BAD_POSITION, "read out port = ERR_BAD_POSITION");
    CHECK(comp("ON BTN\n    BTN -> GVAR[0]\nEND\n")->code == ERR_BAD_POSITION, "read handler source = ERR_BAD_POSITION");

    /* 産出型の追跡: str産出ポートを int スロットへ → ERR_TYPE_MISMATCH（v0.3.8） */
    CHECK(comp("INIT\n    READER -> VAR[0]\nEND\n")->code == ERR_TYPE_MISMATCH, "str port -> int slot = ERR_TYPE_MISMATCH");
    CHECK(comp("INIT\n    READER + 1 -> GVAR[0]\nEND\n")->code == ERR_TYPE_MISMATCH, "str port in arithmetic = ERR_TYPE_MISMATCH");
    CHECK(comp("INIT\n    READER -> SVAR[0]\nEND\n")->code == ERR_NONE, "str port -> str slot compiles (M3)");
    /* str産出ポートの読みは1リストに1個まで（単一SRESULT・M3）。1個＋リテラルはOK、2個は弾く。 */
    CHECK(comp("ON BTN\n    READER, \"x\" -> OUT\nEND\n")->code == ERR_NONE, "1 str port + literal OK");
    CHECK(comp("ON BTN\n    READER, READER2 -> OUT\nEND\n")->code == ERR_BAD_POSITION, "2 str port reads in one list rejected");

    /* END 欠落 → ERR_END_EXPECTED（aux=開きヘッダ行） */
    e = comp("INIT\n    1 -> OUT\n");
    CHECK(e->code == ERR_END_EXPECTED && e->aux == 1, "ERR_END_EXPECTED aux=1 (INIT opened @1)");
    e = comp("ON BTN\n    1 -> OUT\n    (1) -> IFYES\n        1 -> OUT\n");  /* IFYES未閉じ */
    CHECK(e->code == ERR_END_EXPECTED && e->aux == 3, "ERR_END_EXPECTED aux=3 (IFYES opened @3)");

    /* 対応ヘッダの無い END → ERR_UNEXPECTED_END */
    e = comp("MAIN\nEND\nEND\n");
    CHECK(e->code == ERR_UNEXPECTED_END && e->line == 3, "ERR_UNEXPECTED_END @3");

    /* ネスト超過 → ERR_NEST_TOO_DEEP */
    e = comp("INIT\n(1)->IFYES\n (1)->IFYES\n  (1)->IFYES\n   (1)->IFYES\n    (1)->IFYES\n"
             "     1 -> OUT\n    END\n   END\n  END\n END\nEND\nEND\n");
    CHECK(e->code == ERR_NEST_TOO_DEEP, "ERR_NEST_TOO_DEEP");

    /* 未定義ON源 → ERR_UNKNOWN_NAME / def_handlerでない → ERR_SYNTAX */
    e = comp("ON NOPE\n    1 -> OUT\nEND\n");
    CHECK(e->code == ERR_UNKNOWN_NAME && e->line == 1 && strcmp(e->tok,"NOPE")==0, "undefined ON source = ERR_UNKNOWN_NAME @1");
    e = comp("ON LED\n    1 -> OUT\nEND\n");
    CHECK(e->code == ERR_SYNTAX && e->line == 1, "ON non-handler port = ERR_SYNTAX @1");

    /* 字句崩れ → ERR_SYNTAX */
    CHECK(comp("INIT\n    \"oops -> OUT\nEND\n")->code == ERR_SYNTAX, "unterminated string = ERR_SYNTAX");
    CHECK(comp("ON\n    1 -> OUT\nEND\n")->code == ERR_SYNTAX, "empty ON trigger = ERR_SYNTAX");

    /* 直しての再ロードが通る（状態が壊れていない・最初の1個で停止） */
    CHECK(comp("INIT\n    1 -> OUT\nEND\n")->code == ERR_NONE, "valid compile after errors");

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE5 FAILED" : "PHASE5 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
