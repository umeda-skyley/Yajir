/* test_phase7_strings.c - フェーズ7（文字列スロット＋標準Utility）の単体テスト（v0.3 §3,§4）
 *
 * SVAR/SGVAR/SRESULT の代入・コピー・読み出し、型分離（コンパイルエラー）、
 * SLICER/MERGER/COUNTER/FORMATTER/EQUALS、切り詰め＋ERR_STR_TRUNC を検証する。
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
/* 産出none の void out（STDOUT 相当）。RESULT/SRESULT を触らないことの検証用。 */
static void sink_out(int argc, const script_value_t *a){ (void)argc; (void)a; }
/* str産出の入力ポート（READER）。get_fn が産んだ文字列を SRESULT に置く（§3, M3）。 */
static int32_t reader_get(void){ script_set_sresult("HELLO-READER"); return 0; }

static void run_init(void){
    int i; for (i=0;i<vm()->nblocks;i++) if (vm()->blocks[i].kind==BLK_INIT){
        uint16_t pc=vm()->blocks[i].bc_start; int b=CFG_INSTR_BUDGET; int32_t ms=0;
        vm()->sp=0; vm_exec(&pc,&b,&ms,0); return; }
}
static int compile(const char *s){ return script_load(s, strlen(s)); }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in("NOW", now_get, SCRIPT_T_INT);
    script_register_in("READER", reader_get, SCRIPT_T_STR);   /* str産出の入力ポート（M3） */
    script_register_out("SINK", sink_out);   /* 産出none の out（RESULT非更新の検証用） */
    printf("== Phase7 string slots + utilities ==\n");

    /* 1) リテラル代入・スロット間コピー・永続保存 */
    {
        const char *src =
            "INIT\n"
            "    \"HELLO\" -> SVAR[0]\n"     /* リテラル代入 */
            "    SVAR[0] -> SVAR[1]\n"       /* スロット間コピー（strcpy） */
            "    SVAR[0] -> SVAR[0]\n"       /* 自己コピー（安全） */
            "    SVAR[1] -> SGVAR[0]\n"      /* 永続スロットへ保存 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "string assign/copy compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->svar[0], "HELLO")==0, "\"HELLO\" -> SVAR[0]");
            CHECK(strcmp(vm()->svar[1], "HELLO")==0, "SVAR[0] -> SVAR[1] copy");
            CHECK(strcmp(vm()->sgvar[0],"HELLO")==0, "SVAR[1] -> SGVAR[0] persist");
        }
    }

    /* 2) SLICER / MERGER / COUNTER / EQUALS */
    {
        const char *src =
            "INIT\n"
            "    \"HELLO, WORLD!\", 3, 5 -> SLICER\n"   /* SRESULT="LLO, " (1始まり) */
            "    SRESULT -> SGVAR[0]\n"
            "    \"NO MUSIC \", \", NO LIFE\" -> MERGER\n"
            "    SRESULT -> SGVAR[1]\n"
            "    \"HELLO\" -> COUNTER\n"                 /* RESULT=5 */
            "    RESULT -> GVAR[0]\n"
            "    \"ON\", \"ON\" -> EQUALS\n"             /* RESULT=1 */
            "    RESULT -> GVAR[1]\n"
            "    \"ON\", \"OFF\" -> EQUALS\n"            /* RESULT=0 */
            "    RESULT -> GVAR[2]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "utility script compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->sgvar[0], "LLO, ")==0, "SLICER 1-based (3,5) -> \"LLO, \"");
            CHECK(strcmp(vm()->sgvar[1], "NO MUSIC , NO LIFE")==0, "MERGER concat");
            CHECK(vm()->gvar[0].i==5, "COUNTER \"HELLO\"==5");
            CHECK(vm()->gvar[1].i==1, "EQUALS ON==ON ->1");
            CHECK(vm()->gvar[2].i==0, "EQUALS ON==OFF ->0");
        }
    }

    /* 3) FORMATTER（%s %d %x %c %%） */
    {
        const char *src =
            "INIT\n"
            "    \"My name is %s, %d years old\", \"jiro\", 25 -> FORMATTER\n"
            "    SRESULT -> SGVAR[0]\n"
            "    \"hex=%x c=%c %%\", 255, 65 -> FORMATTER\n"
            "    SRESULT -> SGVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "FORMATTER script compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->sgvar[0], "My name is jiro, 25 years old")==0, "FORMATTER %s %d");
            CHECK(strcmp(vm()->sgvar[1], "hex=ff c=A %")==0, "FORMATTER %x %c %%");
        }
    }

    /* 4) 文字列分岐（EQUALS -> RESULT -> IFYES） */
    {
        const char *src =
            "INIT\n"
            "    \"ON\" -> SVAR[0]\n"
            "    \"ON\", SVAR[0] -> EQUALS\n"
            "    (RESULT) -> IFYES\n"
            "        1 -> GVAR[0]\n"
            "    ELSE\n"
            "        9 -> GVAR[0]\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "string-branch script compiles");
        if (r==0){ run_init(); CHECK(vm()->gvar[0].i==1, "EQUALS->IFYES THEN taken"); }
    }

    /* 5) 型分離：コンパイルエラー（v0.3.7 構造化コード） */
    CHECK(compile("INIT\n    123 -> SVAR[0]\nEND\n") == ERR_TYPE_MISMATCH,  "int -> string slot rejected");
    CHECK(compile("INIT\n    \"x\" -> VAR[0]\nEND\n") == ERR_TYPE_MISMATCH, "string -> int slot rejected");
    CHECK(compile("INIT\n    SVAR[0] + 1 -> GVAR[0]\nEND\n") == ERR_TYPE_MISMATCH, "string in arithmetic rejected");
    CHECK(compile("INIT\n    (SVAR[0] > 1) -> IFYES\n 1->GVAR[0]\n END\nEND\n") == ERR_TYPE_MISMATCH, "string in compare rejected");
    CHECK(compile("INIT\n    \"x\" -> SRESULT\nEND\n") == ERR_SYNTAX,       "send to SRESULT rejected");
    { int r=compile("INIT\n    \"x\" -> SVAR[9]\nEND\n");
      CHECK(r==ERR_BAD_SLOT_INDEX && script_last_error()->line==2, "SVAR[9] out of range @2"); }

    /* 6) 切り詰め＋ERR_STR_TRUNC（バッファ64バイト超） */
    {
        const char *src =
            "INIT\n"
            "    \"0123456789012345678901234567890123456789012345678901234567890123456789\" -> SVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "long-string script compiles");
        if (r==0){ run_init();
            CHECK((int)strlen(vm()->svar[0])==CFG_SSTR_LEN-1, "truncated to buffer-1");
            CHECK((script_get_status() & ERR_STR_TRUNC)!=0, "ERR_STR_TRUNC set on truncation");
        }
    }

    /* 7) RESULT規則の反転（v0.3.8）: 産出none の out への送信は RESULT を潰さない。
     *    実機デバッグで踏んだ罠（RESULT -> STDOUT で RESULT が 0 になる）の回帰固定。
     *    COUNTER(産出int) で RESULT を作り、SINK(void out) へ送っても保持されること。 */
    {
        const char *src =
            "INIT\n"
            "    \"HELLO\" -> COUNTER\n"   /* RESULT = 5 */
            "    RESULT -> SINK\n"          /* 産出none out。RESULT を触らないはず */
            "    RESULT -> GVAR[0]\n"       /* 旧仕様なら 0、新仕様なら 5 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "out-preserves-RESULT script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==5, "RESULT survives `-> SINK` (out is 産出none, v0.3.8)");
        }
    }

    /* 8) str入力ポートの実体読み（M3・新opcode無し）: READER(str産出) を読むと、
     *    産んだ文字列（SRESULT 経由）がスタックに積まれ、SVAR へコピーできる。 */
    {
        const char *src =
            "INIT\n"
            "    READER -> SVAR[0]\n"      /* 産んだ文字列を SVAR[0] へ */
            "    SVAR[0] -> SGVAR[0]\n"    /* 永続スロットへ保存 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "str input port read compiles (M3)");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->svar[0], "HELLO-READER")==0, "READER -> SVAR[0] copies produced string");
            CHECK(strcmp(vm()->sgvar[0],"HELLO-READER")==0, "and persists via SGVAR[0]");
        }
    }

    /* 9) FINDER（位置検索・1始まり・無しは0, v0.3.8） */
    {
        const char *src =
            "INIT\n"
            "    \"this is a pen\", \"is\" -> FINDER\n"    /* \"th[is]...\" → 3 */
            "    RESULT -> GVAR[0]\n"
            "    \"this is a pen\", \"xyz\" -> FINDER\n"   /* 無し → 0 */
            "    RESULT -> GVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "FINDER script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==3, "FINDER \"is\" in \"this is a pen\" == 3 (1-based)");
            CHECK(vm()->gvar[1].i==0, "FINDER not found == 0");
        }
    }

    /* 9b) STRTOL（文字列→整数・既定16進・第2引数で基数, v0.4） */
    {
        const char *src =
            "INIT\n"
            "    \"00FF\" -> STRTOL\n"          /* 既定base16 → 255 */
            "    RESULT -> GVAR[0]\n"
            "    \"100\", 10 -> STRTOL\n"        /* base10 → 100 */
            "    RESULT -> GVAR[1]\n"
            "    \"100\", 2 -> STRTOL\n"         /* base2 → 4 */
            "    RESULT -> GVAR[2]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "STRTOL script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==255, "STRTOL \"00FF\" (base16) == 255");
            CHECK(vm()->gvar[1].i==100, "STRTOL \"100\",10 == 100");
            CHECK(vm()->gvar[2].i==4,   "STRTOL \"100\",2 == 4");
        }
    }

    /* 9c) FIELD（区切りN番目トークン・1始まり・範囲外は""・略記は空白区切り, v0.4） */
    {
        const char *src =
            "INIT\n"
            "    \"SEND 0001 0002 OK\", \" \", 3 -> FIELD\n"   /* 3番目 → "0002" */
            "    SRESULT -> SGVAR[0]\n"
            "    \"a,,b,c\", \",\", 2 -> FIELD\n"               /* 連続区切り潰す → "b" */
            "    SRESULT -> SGVAR[1]\n"
            "    \"  hello   world  \", 2 -> FIELD\n"           /* 略記=空白 → "world" */
            "    SRESULT -> SGVAR[2]\n"
            "    \"a b\", \" \", 9 -> FIELD\n"                  /* 範囲外 → "" */
            "    SRESULT -> SGVAR[3]\n"
            "    \"SEND 0001 0002 OK\", \" \", 2 -> FIELD\n"    /* FIELD->STRTOL 連携 */
            "    SRESULT -> STRTOL\n"                          /* "0001"(16) → 1 */
            "    RESULT -> GVAR[0]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "FIELD script compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->sgvar[0],"0002")==0, "FIELD 3rd token == \"0002\"");
            CHECK(strcmp(vm()->sgvar[1],"b")==0,    "FIELD collapses runs == \"b\"");
            CHECK(strcmp(vm()->sgvar[2],"world")==0,"FIELD whitespace shorthand == \"world\"");
            CHECK(strcmp(vm()->sgvar[3],"")==0,     "FIELD out-of-range == \"\"");
            CHECK(vm()->gvar[0].i==1,                       "FIELD->STRTOL \"0001\" == 1");
        }
    }

    /* 9d) UPPER / LOWER / TRIMMER（str産出, v0.4） */
    {
        const char *src =
            "INIT\n"
            "    \"Send_Ok\" -> UPPER\n"
            "    SRESULT -> SGVAR[0]\n"                        /* "SEND_OK" */
            "    \"Send_Ok\" -> LOWER\n"
            "    SRESULT -> SGVAR[1]\n"                        /* "send_ok" */
            "    \"  hi there  \" -> TRIMMER\n"
            "    SRESULT -> SGVAR[2]\n"                        /* "hi there"（中間保持） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "UPPER/LOWER/TRIMMER compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->sgvar[0],"SEND_OK")==0, "UPPER == \"SEND_OK\"");
            CHECK(strcmp(vm()->sgvar[1],"send_ok")==0, "LOWER == \"send_ok\"");
            CHECK(strcmp(vm()->sgvar[2],"hi there")==0,"TRIMMER strips edges, keeps middle");
        }
    }

    /* 10) FORMATTER の幅/フラグ/精度（snprintf 委譲, v0.3.8）。%n %f は無害化（literal）。 */
    {
        const char *src =
            "INIT\n"
            "    \"%4d|%-4d|%08x|%+d|%%\", 42, 7, 255, 9 -> FORMATTER\n"
            "    SRESULT -> SGVAR[0]\n"
            "    \"safe %n %f end\" -> FORMATTER\n"   /* %n/%f は実行されずそのまま出る */
            "    SRESULT -> SGVAR[1]\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "FORMATTER width/flags script compiles");
        if (r==0){ run_init();
            CHECK(strcmp(vm()->sgvar[0], "  42|7   |000000ff|+9|%")==0, "FORMATTER %4d %-4d %08x %+d %% (snprintf-delegated)");
            CHECK(strcmp(vm()->sgvar[1], "safe %n %f end")==0, "FORMATTER neutralizes %n / %f (emitted literally)");
        }
    }

    /* 11) エイリアス（def_alias, v0.3.8）: スロット/数値/文字列の名前解決（コンパイル時・実行時ゼロ） */
    {
        const char *src =
            "def_alias(TRX, GVAR[6])\n"           /* GVAR スロット別名（読み書き可） */
            "def_alias(LIMIT, 99)\n"              /* 数値定数別名（読取専用） */
            "def_alias(GREET, \"HI-ALIAS\")\n"    /* 文字列定数別名（読取専用・プール1回intern） */
            "def_alias(MSG, SGVAR[2])\n"          /* SGVAR スロット別名 */
            "INIT\n"
            "    5 -> TRX\n"                       /* GVAR[6] = 5 */
            "    TRX + LIMIT -> GVAR[0]\n"         /* 5 + 99 = 104 */
            "    GREET -> MSG\n"                   /* SGVAR[2] = "HI-ALIAS" */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "alias script compiles (slot/int/str)");
        if (r==0){ run_init();
            CHECK(vm()->gvar[6].i==5,   "5 -> TRX (GVAR[6]) == 5");
            CHECK(vm()->gvar[0].i==104, "TRX + LIMIT == 104 (slot+int alias)");
            CHECK(strcmp(vm()->sgvar[2],"HI-ALIAS")==0, "GREET -> MSG (str alias -> SGVAR[2])");
        }
    }

    /* 12) エイリアスの制約（コンパイルエラー） */
    CHECK(compile("def_alias(X, VAR[0])\nINIT\n 1->GVAR[0]\nEND\n") == ERR_SYNTAX, "VAR not aliasable (G系のみ)");
    CHECK(compile("def_alias(NOW, GVAR[0])\nINIT\n 1->GVAR[0]\nEND\n") == ERR_SYNTAX, "alias name collides with port (NOW)");
    CHECK(compile("def_alias(GVAR, GVAR[0])\nINIT\n 1->GVAR[0]\nEND\n") == ERR_SYNTAX, "alias name collides with reserved (GVAR)");
    CHECK(compile("def_alias(LIMIT, 99)\nINIT\n 1 -> LIMIT\nEND\n") == ERR_BAD_POSITION, "send to numeric alias = ERR_BAD_POSITION");
    CHECK(compile("def_alias(G, \"x\")\nINIT\n \"y\" -> G\nEND\n") == ERR_BAD_POSITION, "send to string alias = ERR_BAD_POSITION");

    /* 13) パイプライン・チェイン（v0.4）: 継ぎ目は前段産出(RESULT/SRESULT)を流す純パイプ。
     *     鎖は複数文と同一結果（コンパイル時糖衣・新opなし）。 */
    {
        const char *src =
            "INIT\n"
            "    \"HELLO\" -> COUNTER -> GVAR[0]\n"                            /* int産出→GVAR: 5 */
            "    \"NO MUSIC \", \", NO LIFE\" -> MERGER -> SGVAR[0]\n"         /* str産出→SGVAR */
            "    \"NO MUSIC \", \", NO LIFE\" -> MERGER -> COUNTER -> GVAR[1]\n" /* 3段 str→int: 18 */
            "    \"ON\", \"ON\" -> EQUALS -> IFYES\n"                          /* 終端=IFYES（int産出→条件） */
            "        7 -> GVAR[2]\n"
            "    END\n"
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "chain script compiles (v0.4)");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==5,  "\"HELLO\"->COUNTER->GVAR[0] == 5 (terminal slot)");
            CHECK(strcmp(vm()->sgvar[0],"NO MUSIC , NO LIFE")==0, "MERGER->SGVAR[0] (str seam)");
            CHECK(vm()->gvar[1].i==18, "MERGER->COUNTER->GVAR[1] == 18 (3-stage str->int)");
            CHECK(vm()->gvar[2].i==7,  "EQUALS->IFYES THEN taken (terminal IFYES)");
        }
    }

    /* 14) チェインの位置/型規則 */
    CHECK(compile("INIT\n    \"x\" -> SINK -> GVAR[0]\nEND\n") == ERR_BAD_POSITION, "out port in chain middle = ERR_BAD_POSITION");
    CHECK(compile("INIT\n    \"a\",\"b\" -> MERGER -> VAR[0]\nEND\n") == ERR_TYPE_MISMATCH, "str product into int slot terminal = ERR_TYPE_MISMATCH");
    CHECK(compile("INIT\n    5 -> GVAR[0] -> COUNTER\nEND\n") == ERR_BAD_POSITION, "slot in chain middle = ERR_BAD_POSITION");

    /* 15) 組込み入力ポート（資源使用量・バージョン, v0.4） */
    {
        const char *src =
            "INIT\n"
            "    \"hi\" -> SVAR[1]\n"        /* 文字列プールに \"hi\" を積む */
            "    CODE_USED -> GVAR[0]\n"     /* バイトコード使用量 */
            "    STR_USED  -> GVAR[1]\n"     /* 文字列プール使用量 */
            "    VERSION   -> SVAR[0]\n"     /* バージョン文字列（str産出inポート=M3経路） */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "builtin resource/version ports compile");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i == (int32_t)vm()->code_len && vm()->code_len > 0, "CODE_USED == code_len (>0)");
            CHECK(vm()->gvar[1].i == (int32_t)vm()->strpool_len && vm()->strpool_len > 0, "STR_USED == strpool_len (>0)");
            CHECK(strcmp(vm()->svar[0], SCRIPT_VERSION)==0, "VERSION -> SVAR[0] == version string");
        }
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE7 FAILED" : "PHASE7 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
