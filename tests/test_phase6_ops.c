/* test_phase6_ops.c - フェーズ6（演算拡張・数値リテラル・STATUS）の単体テスト（v0.3 §9,§12）
 *
 * * / % と & | ^ ~ << >> 単項-、16進/2進/桁区切りリテラル、優先順位、
 * /0 %0 → 0継続＋ERR_DIVZERO、STATUS 読み／CLEAR_ERR を検証する。
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
    printf("== Phase6 operators / literals / STATUS ==\n");

    /* 1) 算術・ビット・単項・優先順位 */
    {
        const char *src =
            "INIT\n"
            "    7 * 6 -> GVAR[0]\n"            /* 42 */
            "    20 / 6 -> GVAR[1]\n"           /* 3  */
            "    20 % 6 -> GVAR[2]\n"           /* 2  */
            "    1 << 4 -> GVAR[3]\n"           /* 16 */
            "    0xF0 >> 4 -> GVAR[4]\n"        /* 15 */
            "    (2 + 3) * 4 -> GVAR[5]\n"      /* 20 (括弧優先) */
            "    2 + 3 * 4 -> GVAR[6]\n"        /* 14 (*が+より上) */
            "    ~0 -> GVAR[7]\n"               /* -1 */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "arith/bit script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==42, "7*6==42");
            CHECK(vm()->gvar[1].i==3,  "20/6==3");
            CHECK(vm()->gvar[2].i==2,  "20%6==2");
            CHECK(vm()->gvar[3].i==16, "1<<4==16");
            CHECK(vm()->gvar[4].i==15, "0xF0>>4==15");
            CHECK(vm()->gvar[5].i==20, "(2+3)*4==20");
            CHECK(vm()->gvar[6].i==14, "2+3*4==14 (mul before add)");
            CHECK(vm()->gvar[7].i==-1, "~0==-1");
        }
    }

    /* 2) ビット演算の優先順位（& < ^ < | < AND < OR、比較は&より上） */
    {
        const char *src =
            "INIT\n"
            "    0xFF & 0x0F -> GVAR[0]\n"      /* 15 */
            "    0b1010 | 0b0101 -> GVAR[1]\n"  /* 15 */
            "    6 ^ 3 -> GVAR[2]\n"            /* 5  */
            "    1 | 2 & 0 -> GVAR[3]\n"        /* & が | より上 → 1|(2&0)=1 */
            "    5 > 3 & 1 -> GVAR[4]\n"        /* 比較が & より上 → (5>3)&1=1 */
            "    -5 -> GVAR[5]\n"               /* 単項マイナス */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "bit-precedence script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==15, "0xFF & 0x0F==15");
            CHECK(vm()->gvar[1].i==15, "0b1010|0b0101==15");
            CHECK(vm()->gvar[2].i==5,  "6^3==5");
            CHECK(vm()->gvar[3].i==1,  "1|2&0==1 (& over |)");
            CHECK(vm()->gvar[4].i==1,  "5>3&1==1 (cmp over &)");
            CHECK(vm()->gvar[5].i==-5, "unary -5==-5");
        }
    }

    /* 3) リテラル：16進/2進/桁区切り、0xFFFFFFFF=-1 */
    {
        const char *src =
            "INIT\n"
            "    0xFF -> GVAR[0]\n"             /* 255 */
            "    0b1010 -> GVAR[1]\n"           /* 10 */
            "    1_000_000 -> GVAR[2]\n"        /* 1000000 */
            "    0xFFFF_FFFF -> GVAR[3]\n"      /* -1 (32bitビットパターン) */
            "    0XfF -> GVAR[4]\n"            /* 255 (大小文字可) */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "literal script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==255,     "0xFF==255");
            CHECK(vm()->gvar[1].i==10,      "0b1010==10");
            CHECK(vm()->gvar[2].i==1000000, "1_000_000==1000000");
            CHECK(vm()->gvar[3].i==-1,      "0xFFFF_FFFF==-1");
            CHECK(vm()->gvar[4].i==255,     "0XfF==255");
        }
    }

    /* 4) 33bit以上リテラルはコンパイルエラー */
    {
        CHECK(compile("INIT\n    0x1_0000_0000 -> GVAR[0]\nEND\n") != 0, "0x100000000 (33bit) rejected");
    }

    /* 5) /0 %0 → 0継続＋ERR_DIVZERO、STATUS読み、CLEAR_ERR */
    {
        const char *src =
            "INIT\n"
            "    10 / 0 -> GVAR[0]\n"                  /* 0 + ERR_DIVZERO */
            "    (STATUS & ERR_DIVZERO) -> GVAR[1]\n"  /* 4 (立っている) */
            "    ERR_DIVZERO -> CLEAR_ERR\n"
            "    (STATUS & ERR_DIVZERO) -> GVAR[2]\n"  /* 0 (クリア後) */
            "    10 % 0 -> GVAR[3]\n"                  /* 0 + 再度 ERR_DIVZERO */
            "END\n";
        int r = compile(src);
        CHECK(r == 0, "divzero/STATUS script compiles");
        if (r==0){ run_init();
            CHECK(vm()->gvar[0].i==0, "10/0==0 (continue)");
            CHECK(vm()->gvar[1].i==ERR_DIVZERO, "STATUS&ERR_DIVZERO set after /0");
            CHECK(vm()->gvar[2].i==0, "ERR_DIVZERO cleared by CLEAR_ERR");
            CHECK(vm()->gvar[3].i==0, "10%0==0 (continue)");
            CHECK((script_get_status() & ERR_DIVZERO)!=0, "status shows DIVZERO after %0");
        }
    }

    printf("\n%s (failures=%d)\n", g_fail ? "PHASE6 FAILED" : "PHASE6 PASSED", g_fail);
    return g_fail ? 1 : 0;
}
