/* test_phase8_multiarg.c - フェーズ8（多値イベント＋SARG＋HANDLER多値, v0.3.5/3.6）
 *
 * script_post_msg_v の混在多値、ARG/SARG の同位置・型振り分け2ビュー、
 * 文字列引数の SARG コピー＋切詰、SARG受信専用、式リスト->HANDLER 多値非再帰 を検証。
 */
#include <stdio.h>
#include <string.h>
#include "../src/core/script.h"
#include "../src/core/vm.h"

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else { printf("  FAIL : %s\n", msg); g_fail++; } } while (0)

static int32_t g_now = 0;
static int32_t now_get(void){ return g_now; }

/* OUT は int値 or 文字列を順に記録 */
static int     g_n = 0;
static int     g_isstr[64];
static int32_t g_ival[64];
static char    g_sval[64][48];
static void out_rec(int argc, const script_value_t *a){
    int i;
    for (i = 0; i < argc; i++) {
        if (script_val_is_str(a[i])) { g_isstr[g_n]=1; strncpy(g_sval[g_n], script_resolve_str(a[i]), 47); g_sval[g_n][47]=0; }
        else                         { g_isstr[g_n]=0; g_ival[g_n]=a[i].i; }
        g_n++;
    }
}
static void tick_at(int32_t t){ g_now=t; script_tick(); }
static int compile(const char *s){ return script_load(s, strlen(s)); }

static void setup(void){
    static char arena[sizeof(script_vm_t)+128];
    script_init(arena, sizeof(arena));
    script_register_in ("NOW", now_get, SCRIPT_T_INT);
    script_register_out("OUT", out_rec);
    script_register_handler("RX");
    script_register_handler("MYHANDLER");   /* 名前付きハンドラ（v0.3.7+）: glue は register_handler 1行 */
}

int main(void)
{
    setup();
    printf("== Phase8 multi-arg events + SARG + HANDLER ==\n");

    /* 1) 混在多値 post → ARG/SARG の型振り分け2ビュー */
    {
        const char *src =
            "ON RX\n"
            "    ARG[0]  -> OUT\n"   /* pos0=int42 → 42 */
            "    SARG[1] -> OUT\n"   /* pos1=str   → "hello" */
            "    ARG[1]  -> OUT\n"   /* pos1はstr → ARG[1] benign 0 */
            "    SARG[0] -> OUT\n"   /* pos0はint → SARG[0] benign "" */
            "END\n";
        script_arg_t args[2]; args[0]=SCRIPT_ARG_INT(42); args[1]=SCRIPT_ARG_STR("hello", strlen("hello"));
        CHECK(compile(src)==0, "ON RX multi-arg script compiles");
        g_n=0; tick_at(0);
        CHECK(script_post_msg_v("RX", 2, args)==0, "post_msg_v queued");
        tick_at(1);
        CHECK(g_n==4, "handler emitted 4 values");
        CHECK(g_isstr[0]==0 && g_ival[0]==42, "ARG[0] int view == 42");
        CHECK(g_isstr[1]==1 && strcmp(g_sval[1],"hello")==0, "SARG[1] str view == hello");
        CHECK(g_isstr[2]==0 && g_ival[2]==0, "ARG[1] benign 0 (pos is str)");
        CHECK(g_isstr[3]==1 && g_sval[3][0]=='\0', "SARG[0] benign empty (pos is int)");
    }

    /* 2) 単値 post は従来どおり ARG[0] */
    {
        const char *src = "ON RX\n    ARG[0] -> OUT\nEND\n";
        compile(src); g_n=0; tick_at(0);
        script_post_msg("RX", 99); tick_at(1);
        CHECK(g_n==1 && g_ival[0]==99, "post_msg single int -> ARG[0]==99");
    }

    /* 3) 式リスト -> HANDLER（多値・非再帰）→ ON HANDLER。
     *    INIT中のpostは破棄される（v0.3.4）ので、起点は ON RX（RUNフェーズ）に置く。 */
    {
        const char *src =
            "ON RX\n"
            "    7, \"hi\" -> HANDLER\n"   /* 即post（その場で本体に降りない、次tickでHANDLER） */
            "END\n"
            "ON HANDLER\n"
            "    ARG[0]  -> OUT\n"         /* 7 */
            "    SARG[1] -> OUT\n"         /* "hi" */
            "END\n";
        CHECK(compile(src)==0, "HANDLER multi-value script compiles");
        g_n=0; tick_at(0);                 /* INIT→RUN */
        script_post_msg("RX", 0);
        tick_at(1);                        /* RX→HANDLERをpost（次tickへ） */
        tick_at(2);                        /* HANDLER発火 */
        CHECK(g_n==2 && g_ival[0]==7 && strcmp(g_sval[1],"hi")==0, "HANDLER carried (7, \"hi\")");
    }

    /* 4) 受信文字列を keep: SARG[0] -> SVAR[0] はOK / SARGへ書くのはエラー */
    {
        const char *src =
            "ON RX\n"
            "    SARG[0] -> SVAR[0]\n"     /* 受信文字列を保持用へコピー */
            "    SVAR[0] -> OUT\n"
            "END\n";
        script_arg_t a1[1]; a1[0]=SCRIPT_ARG_STR("keepme", strlen("keepme"));
        CHECK(compile(src)==0, "SARG[0]->SVAR[0] compiles (read source)");
        g_n=0; tick_at(0); script_post_msg_v("RX", 1, a1); tick_at(1);
        CHECK(g_n==1 && strcmp(g_sval[0],"keepme")==0, "received str copied to SVAR[0]");

        CHECK(compile("ON RX\n    \"x\" -> SARG[0]\nEND\n") != 0, "write to SARG rejected (receive-only)");
        CHECK(compile("ON RX\n    SARG[0] -> SARG[1]\nEND\n") != 0, "SARG[0]->SARG[1] rejected (dest receive-only)");
    }

    /* 5) 文字列引数の切り詰め＋ERR_STR_TRUNC（SARG_LEN=32超） */
    {
        const char *src = "ON RX\n    SARG[0] -> OUT\nEND\n";
        script_arg_t a1[1];
        a1[0]=SCRIPT_ARG_STR("0123456789012345678901234567890123456789", 40); /* 40字（明示長） */
        compile(src); g_n=0; tick_at(0);
        script_post_msg_v("RX", 1, a1); tick_at(1);
        CHECK(g_n==1 && (int)strlen(g_sval[0])==CFG_SARG_LEN-1, "SARG truncated to SARG_LEN-1");
        CHECK((script_get_status() & ERR_STR_TRUNC)!=0, "ERR_STR_TRUNC set on arg truncation");
    }

    /* 6) (int,int,str) RXDATAパターン：位置2の文字列は N_SARG>=3 で SARG[2] に入る */
    {
        const char *src =
            "ON RX\n"
            "    ARG[0]  -> OUT\n"     /* src  */
            "    ARG[1]  -> OUT\n"     /* dst  */
            "    SARG[2] -> OUT\n"     /* data */
            "END\n";
        script_arg_t a[3]; a[0]=SCRIPT_ARG_INT(5); a[1]=SCRIPT_ARG_INT(0x12); a[2]=SCRIPT_ARG_STR("PING", strlen("PING"));
        compile(src); g_n=0; tick_at(0);
        script_post_msg_v("RX", 3, a); tick_at(1);
        CHECK(g_n==3 && g_ival[0]==5 && g_ival[1]==0x12 && strcmp(g_sval[2],"PING")==0,
              "(int,int,str): ARG0=5, ARG1=0x12, SARG[2]=\"PING\"");
    }

    /* 7) self-post 反復は「1 tick 1ステップ」で進む（v0.3.6 「次tick以降」）。
     *    無限self-postでも1 tick内で固まらない（drain境界スナップショット）。 */
    {
        const char *src =
            "ON RX\n"
            "    5 -> HANDLER\n"                  /* 連鎖の起点を1発post */
            "END\n"
            "ON HANDLER\n"
            "    (ARG[0] > 0) -> IFYES\n"
            "        ARG[0] + GVAR[0] -> GVAR[0]\n"
            "        (ARG[0] - 1) -> HANDLER\n"   /* 自己post（次tickへ） */
            "    END\n"
            "END\n";
        compile(src); tick_at(0);
        script_post_msg("RX", 0);
        tick_at(1); CHECK(vm()->gvar[0].i==0,  "RX posts HANDLER but it defers to next tick (0)");
        tick_at(2); CHECK(vm()->gvar[0].i==5,  "step1: +5");
        tick_at(3); CHECK(vm()->gvar[0].i==9,  "step2: +4");
        tick_at(4); CHECK(vm()->gvar[0].i==12, "step3: +3");
        tick_at(5); CHECK(vm()->gvar[0].i==14, "step4: +2");
        tick_at(6); CHECK(vm()->gvar[0].i==15, "step5: +1 (one step per tick)");
        tick_at(7); CHECK(vm()->gvar[0].i==15, "terminates, stable");
    }

    /* 8) 名前付きHANDLER（v0.3.7+）: スクリプトから "1,2,3 -> MYHANDLER"、
     *    C から script_post_msg_v("MYHANDLER",...) の両経路が ON MYHANDLER に入る。
     *    HANDLER 同様に「次tickで発火（遅延post）」。glueは register_handler 1行のみ。 */
    {
        const char *src =
            "ON RX\n"
            "    1, 2, 3 -> MYHANDLER\n"   /* スクリプト側ポスト（次tickへ） */
            "END\n"
            "ON MYHANDLER\n"
            "    ARG[0] -> OUT\n"
            "    ARG[1] -> OUT\n"
            "    ARG[2] -> OUT\n"
            "END\n";
        script_arg_t a[3];
        CHECK(compile(src)==0, "named-handler script compiles (-> MYHANDLER, ON MYHANDLER)");

        /* (a) スクリプト側ポスト経路 */
        g_n=0; tick_at(0);
        script_post_msg("RX", 0);
        tick_at(1); CHECK(g_n==0, "script-side -> MYHANDLER defers to next tick");
        tick_at(2); CHECK(g_n==3 && g_ival[0]==1 && g_ival[1]==2 && g_ival[2]==3,
                          "ON MYHANDLER got (1,2,3) from script post");

        /* (b) C側ポスト経路（同じ ON MYHANDLER が受ける） */
        a[0]=SCRIPT_ARG_INT(7); a[1]=SCRIPT_ARG_INT(8); a[2]=SCRIPT_ARG_INT(9);
        g_n=0;
        CHECK(script_post_msg_v("MYHANDLER", 3, a)==0, "C-side post_msg_v to MYHANDLER queued");
        tick_at(3); CHECK(g_n==3 && g_ival[0]==7 && g_ival[2]==9,
                          "ON MYHANDLER got (7,8,9) from C post (same channel)");
    }

    /* 9) INVOKER（動的ディスパッチ・メタ・可変arity）: 文字列名でハンドラを起動＋引数転送。
     *    未登録/ON無しの名前は黙って無視（遅延束縛）。 */
    {
        const char *src =
            "ON RX\n"
            "    \"H1\", 7, \"hi\" -> INVOKER\n"   /* 名前H1へ 7,\"hi\" を転送（次tick） */
            "    \"NOPE\" -> INVOKER\n"            /* ON も登録も無い → 無視（落ちない） */
            "END\n"
            "ON H1\n"
            "    ARG[0]  -> OUT\n"                 /* 7 */
            "    SARG[1] -> OUT\n"                 /* "hi" */
            "END\n";
        script_register_handler("H1");            /* ハンドラ源 H1 を登録（ON H1 用） */
        CHECK(compile(src)==0, "INVOKER script compiles");
        g_n=0; tick_at(0);
        script_post_msg("RX", 0);
        tick_at(1);                                /* RX→INVOKER が H1 をpost（次tickへ） */
        tick_at(2);                                /* H1 発火 */
        CHECK(g_n==2 && g_ival[0]==7 && strcmp(g_sval[1],"hi")==0,
              "INVOKER dispatched to ON H1 with forwarded (7,\"hi\")");
    }

    /* 10) SARG[3]: 位置3の文字列（CFG_SARG_COUNT=4 に拡張, v0.4） */
    {
        const char *src = "ON RX\n    ARG[0] -> OUT\n    SARG[3] -> OUT\nEND\n";
        script_arg_t a[4];
        a[0]=SCRIPT_ARG_INT(11); a[1]=SCRIPT_ARG_INT(22); a[2]=SCRIPT_ARG_INT(33); a[3]=SCRIPT_ARG_STR("P3", 2);
        CHECK(compile(src)==0, "SARG[3] compiles (CFG_SARG_COUNT>=4)");
        g_n=0; tick_at(0); script_post_msg_v("RX", 4, a); tick_at(1);
        CHECK(g_n==2 && g_ival[0]==11 && strcmp(g_sval[1],"P3")==0, "(int,int,int,str): SARG[3]==\"P3\"");
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE8 FAILED" : "PHASE8 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
