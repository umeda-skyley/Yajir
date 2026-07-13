/* test_phase12_ports.c - フェーズ12（スクリプト内ポート / 値付きEXIT）の単体テスト（v0.4.5 §3, §7）
 *
 * def_port + PORT … END のサブルーチン（int/str・チェイン・引数0呼び・ARG/SARG退避復帰・
 * VAR/SVAR共有・ネスト）、値付き `値 -> EXIT` と単独 EXIT（デフォルト値）、および
 * 呼び出しグラフの静的DAG判定（自己/相互再帰=ERR_RECURSION・深度超過=ERR_NEST_TOO_DEEP）と
 * WAIT不可・型不一致などのコンパイルエラーを検証。
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

/* INIT を実走（1 tick で INIT 完走→RUN 遷移） */
static void run(const char *s){ if (compile(s)==0) script_tick(); }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    printf("== Phase12 script ports / EXIT ==\n");

    /* 1) 基本: int サブルーチン（ARG[0]+ARG[1] を返す）＋終端呼び */
    {
        const char *src =
            "def_port(SUM, T_INT)\n"
            "PORT SUM\n"
            "    ARG[0] + ARG[1] -> EXIT\n"
            "END\n"
            "INIT\n"
            "    1, 3 -> SUM -> GVAR[0]\n"       /* 4 */
            "    10, 20 -> SUM -> GVAR[1]\n"     /* 30 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "int PORT compiles");
        if (r==0){ script_tick();
            CHECK(vm()->gvar[0].i == 4,  "1,3 -> SUM == 4");
            CHECK(vm()->gvar[1].i == 30, "10,20 -> SUM == 30");
        }
    }

    /* 2) 引数0の値源読み（SUM 単独左辺）＝ ARG 全0 → 0 */
    {
        const char *src =
            "def_port(SUM, T_INT)\n"
            "PORT SUM\n"
            "    ARG[0] + ARG[1] -> EXIT\n"
            "END\n"
            "INIT\n"
            "    SUM -> GVAR[0]\n"               /* ARG全0 → 0 */
            "END\n";
        run(src);
        CHECK(vm()->gvar[0].i == 0, "bare SUM (argc0, ARG all 0) == 0");
    }

    /* 3) チェイン: port -> port（3^2 を2回 = 81） */
    {
        const char *src =
            "def_port(SQ, T_INT)\n"
            "PORT SQ\n"
            "    ARG[0] * ARG[0] -> EXIT\n"
            "END\n"
            "INIT\n"
            "    3 -> SQ -> GVAR[0]\n"           /* 9 */
            "    3 -> SQ -> SQ -> GVAR[1]\n"     /* (3^2)^2 = 81 */
            "END\n";
        run(src);
        CHECK(vm()->gvar[0].i == 9,  "3 -> SQ == 9");
        CHECK(vm()->gvar[1].i == 81, "3 -> SQ -> SQ == 81");
    }

    /* 4) str サブルーチン: 早期リターン＋MERGERチェイン戻り */
    {
        const char *src =
            "def_port(GREET, T_STR)\n"
            "PORT GREET\n"
            "    \"\", SARG[0] -> EQUALS -> IFYES\n"     /* 空文字判定は EQUALS（== は int 専用） */
            "        \"hello, world\" -> EXIT\n"
            "    END\n"
            "    \"hello, \", SARG[0] -> MERGER -> EXIT\n"
            "END\n"
            "INIT\n"
            "    GREET -> SGVAR[0]\n"                 /* 引数0 → SARG[0]=\"\" → \"hello, world\" */
            "    \"Yajir\" -> GREET -> SGVAR[1]\n"    /* \"hello, Yajir\" */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "str PORT compiles");
        if (r==0){ script_tick();
            CHECK(strcmp(vm()->sgvar[0], "hello, world") == 0, "GREET() early-return == 'hello, world'");
            CHECK(strcmp(vm()->sgvar[1], "hello, Yajir") == 0, "'Yajir' -> GREET == 'hello, Yajir'");
        }
    }

    /* 5) 単独 EXIT＝デフォルト値（int=0 / str=空）＋END到達フォールスルー */
    {
        const char *src =
            "def_port(PICK, T_INT)\n"
            "def_port(TAG, T_STR)\n"
            "PORT PICK\n"
            "    (ARG[0] > 0) -> IFYES\n"
            "        ARG[0] -> EXIT\n"        /* 正なら値 */
            "    END\n"
            "    EXIT\n"                      /* 単独 EXIT ＝ 0 -> EXIT（int既定0） */
            "END\n"
            "PORT TAG\n"
            "    (ARG[0] == 1) -> IFYES\n"
            "        \"one\" -> EXIT\n"
            "    END\n"
            "END\n"                           /* フォールスルー ＝ 空文字 */
            "INIT\n"
            "    5 -> PICK -> GVAR[0]\n"       /* 5 */
            "    -3 -> PICK -> GVAR[1]\n"      /* 単独EXIT → 0 */
            "    1 -> TAG -> SGVAR[0]\n"       /* \"one\" */
            "    2 -> TAG -> SGVAR[1]\n"       /* フォールスルー → \"\" */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "bare EXIT / fallthrough compiles");
        if (r==0){ script_tick();
            CHECK(vm()->gvar[0].i == 5, "PICK(5) == 5");
            CHECK(vm()->gvar[1].i == 0, "PICK(-3) bare EXIT == 0");
            CHECK(strcmp(vm()->sgvar[0], "one") == 0, "TAG(1) == 'one'");
            CHECK(vm()->sgvar[1][0] == '\0', "TAG(2) fallthrough == ''");
        }
    }

    /* 6) VAR は呼び出し元と共有（退避しない） */
    {
        const char *src =
            "def_port(SETV, T_INT)\n"
            "PORT SETV\n"
            "    99 -> VAR[0]\n"              /* 呼び出し元と共有の VAR に書く */
            "    0 -> EXIT\n"
            "END\n"
            "INIT\n"
            "    SETV -> GVAR[0]\n"           /* 呼ぶ（戻り値0は捨てる） */
            "    VAR[0] -> GVAR[1]\n"         /* 共有ゆえ 99 が見える */
            "END\n";
        run(src);
        CHECK(vm()->var[0].i == 99,  "PORT writes shared VAR[0]");
        CHECK(vm()->gvar[1].i == 99, "caller sees VAR[0]==99 after call");
    }

    /* 7) ネスト呼び（A->B->C・深さ3）は CFG_CALL_NEST 以内でコンパイル＆実行 */
    {
        const char *src =
            "def_port(A, T_INT)\n"
            "def_port(B, T_INT)\n"
            "def_port(C, T_INT)\n"
            "PORT C\n"
            "    ARG[0] + 1 -> EXIT\n"
            "END\n"
            "PORT B\n"
            "    ARG[0] -> C -> EXIT\n"       /* C を呼んで返す（B自体は +1 しない） */
            "END\n"
            "PORT A\n"
            "    ARG[0] -> B -> EXIT\n"       /* B を呼んで返す */
            "END\n"
            "INIT\n"
            "    10 -> A -> GVAR[0]\n"        /* A->B->C、+1 は C の1回だけ = 11 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "nested A->B->C compiles (depth 3)");
        if (r==0){ script_tick(); CHECK(vm()->gvar[0].i == 11, "10 -> A -> B -> C == 11"); }
    }

    /* 8) ARG 退避/復帰: ハンドラの ARG が呼び出しで壊れない（VMが自動退避） */
    {
        const char *src =
            "def_port(ADD1, T_INT)\n"
            "def_handler(GO)\n"
            "PORT ADD1\n"
            "    ARG[0] + 1 -> EXIT\n"
            "END\n"
            "ON GO\n"
            "    ARG[0] -> GVAR[0]\n"         /* 受信 ARG[0]=5 を退避前に記録 */
            "    100 -> ADD1 -> GVAR[1]\n"    /* 101。呼び出し中 ARG[0]=100 に上書き */
            "    ARG[0] -> GVAR[2]\n"         /* 復帰していれば 5（壊れていれば 100） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "ARG save/restore case compiles");
        if (r==0){
            script_tick();                    /* INIT→RUN 遷移 */
            script_post_msg("GO", 5);
            script_tick();                    /* GO 発火 */
            CHECK(vm()->gvar[0].i == 5,   "handler ARG[0] captured == 5");
            CHECK(vm()->gvar[1].i == 101, "100 -> ADD1 == 101");
            CHECK(vm()->gvar[2].i == 5,   "ARG[0] restored after call == 5");
        }
    }

    /* 9) SARG 退避/復帰 ＋ SARG参照を引数に渡す（自壊しない） */
    {
        const char *src =
            "def_port(ECHO, T_STR)\n"
            "def_handler(GOS)\n"
            "PORT ECHO\n"
            "    SARG[0] -> EXIT\n"           /* 受けた文字列をそのまま返す */
            "END\n"
            "ON GOS\n"
            "    SARG[0] -> SGVAR[0]\n"       /* 受信 \"hi\" を記録 */
            "    \"xx\" -> ECHO -> SGVAR[1]\n"/* \"xx\"。呼び中 SARG[0]=\"xx\" */
            "    SARG[0] -> SGVAR[2]\n"       /* 復帰していれば \"hi\" */
            "    SARG[0] -> ECHO -> SGVAR[3]\n"/* SARG参照を引数に → \"hi\"（退避元から解決） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "SARG save/restore case compiles");
        if (r==0){
            script_tick();
            script_arg_t a[1]; a[0] = SCRIPT_ARG_STR("hi", 2);
            script_post_msg_v("GOS", 1, a);
            script_tick();
            CHECK(strcmp(vm()->sgvar[0], "hi") == 0, "handler SARG[0] captured == 'hi'");
            CHECK(strcmp(vm()->sgvar[1], "xx") == 0, "'xx' -> ECHO == 'xx'");
            CHECK(strcmp(vm()->sgvar[2], "hi") == 0, "SARG[0] restored after call == 'hi'");
            CHECK(strcmp(vm()->sgvar[3], "hi") == 0, "SARG[0] as arg resolves to 'hi'");
        }
    }

    /* ---- コンパイルエラー系 ---- */

    /* 10) 直接自己再帰 → ERR_RECURSION */
    {
        const char *src =
            "def_port(REC, T_INT)\n"
            "PORT REC\n"
            "    1 -> REC -> EXIT\n"
            "END\n"
            "INIT\n    0 -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == ERR_RECURSION, "direct self-recursion -> ERR_RECURSION");
    }

    /* 11) 相互再帰 A->B->A → ERR_RECURSION */
    {
        const char *src =
            "def_port(F1, T_INT)\n"
            "def_port(F2, T_INT)\n"
            "PORT F1\n"
            "    1 -> F2 -> EXIT\n"
            "END\n"
            "PORT F2\n"
            "    1 -> F1 -> EXIT\n"
            "END\n"
            "INIT\n    0 -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == ERR_RECURSION, "mutual recursion F1<->F2 -> ERR_RECURSION");
    }

    /* 12) PORT 内 WAIT → ERR_WAIT_IN_ON */
    {
        const char *src =
            "def_port(P, T_INT)\n"
            "PORT P\n"
            "    100 -> WAIT\n"
            "    0 -> EXIT\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_WAIT_IN_ON, "WAIT inside PORT -> ERR_WAIT_IN_ON");
    }

    /* 13) EXIT 値の型不一致（T_INT ポートに str） → ERR_TYPE_MISMATCH */
    {
        const char *src =
            "def_port(P, T_INT)\n"
            "PORT P\n"
            "    \"x\" -> EXIT\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_TYPE_MISMATCH, "str -> EXIT in T_INT port -> ERR_TYPE_MISMATCH");
    }

    /* 14) 産出型不一致チェイン（int 産出を str スロットへ） → ERR_TYPE_MISMATCH */
    {
        const char *src =
            "def_port(SUM, T_INT)\n"
            "PORT SUM\n"
            "    ARG[0] + ARG[1] -> EXIT\n"
            "END\n"
            "INIT\n"
            "    1, 3 -> SUM -> SGVAR[0]\n"   /* int産出 → str スロット */
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_TYPE_MISMATCH, "int PORT -> SGVAR -> ERR_TYPE_MISMATCH");
    }

    /* 15) def_port 宣言のみ・本体なし → ERR_SYNTAX */
    {
        const char *src =
            "def_port(NOBODY, T_INT)\n"
            "INIT\n    0 -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "def_port without PORT body -> ERR_SYNTAX");
    }

    /* 16) PORT 本体だが def_port 宣言なし → ERR_SYNTAX */
    {
        const char *src =
            "PORT ORPHAN\n"
            "    0 -> EXIT\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "PORT body without def_port -> ERR_SYNTAX");
    }

    /* 17) 深度超過（A->B->C->D->E＝5段 > CFG_CALL_NEST=4） → ERR_NEST_TOO_DEEP */
    {
        const char *src =
            "def_port(A, T_INT)\n"
            "def_port(B, T_INT)\n"
            "def_port(C, T_INT)\n"
            "def_port(D, T_INT)\n"
            "def_port(E, T_INT)\n"
            "PORT E\n    ARG[0] -> EXIT\nEND\n"
            "PORT D\n    ARG[0] -> E -> EXIT\nEND\n"
            "PORT C\n    ARG[0] -> D -> EXIT\nEND\n"
            "PORT B\n    ARG[0] -> C -> EXIT\nEND\n"
            "PORT A\n    ARG[0] -> B -> EXIT\nEND\n"
            "INIT\n    1 -> A -> GVAR[0]\nEND\n";
        int r = compile(src);
        CHECK(r == ERR_NEST_TOO_DEEP, "call depth 5 > CFG_CALL_NEST -> ERR_NEST_TOO_DEEP");
    }

    /* 18) 単独 EXIT を MAIN で使う → ERR_SYNTAX */
    {
        const char *src =
            "MAIN\n"
            "    EXIT\n"
            "END\n";
        int r = compile(src);
        CHECK(r == ERR_SYNTAX, "bare EXIT in MAIN -> ERR_SYNTAX");
    }

    /* 19) 既存の裸 EXIT がハンドラで従来どおり通る（後方互換） */
    {
        const char *src =
            "def_handler(H)\n"
            "ON H\n"
            "    (ARG[0] == 0) -> IFYES\n"
            "        EXIT\n"                  /* 裸 EXIT（後方互換） */
            "    END\n"
            "    ARG[0] -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "bare EXIT in handler still compiles (backward compat)");
    }

    printf("\n%s (fails=%d)\n", g_fail==0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail ? 1 : 0;
}
