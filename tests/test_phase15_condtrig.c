/* test_phase15_condtrig.c - フェーズ15（条件トリガ `ON <周期> (条件)`）の単体テスト（v0.4.7 §6）
 *
 * エッジ意味論（偽→真で1回・偽で自動リセット・再成立で再発火）、t0直後の初回発火、
 * 条件が偽の間は発火しないこと、レベルでなくエッジであること（真が続いても1回）、
 * 純粋式（ポート読み/スロット式）、型違反を検証。
 * 条件の入力は模擬ADCポートを登録して任意に動かす。
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

/* モッククロック＋模擬ADC（テストから任意に動かす） */
static int32_t g_now = 0, g_adc = 0;
static int32_t now_get(void) { return g_now; }
static int32_t adc_get(void) { return g_adc; }
static void run_ticks(int n, int32_t step) { int i; for (i=0;i<n;i++){ script_tick(); g_now += step; } }

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    script_init(arena, sizeof(arena));
    script_register_in("NOW",  now_get, SCRIPT_T_INT);
    script_register_in("ADC0", adc_get, SCRIPT_T_INT);
    printf("== Phase15 conditional trigger (ON <ms> (cond)) ==\n");

    /* 1) エッジ: 偽→真で1回だけ。真が続いても再発火しない（レベルではない） */
    {
        const char *src =
            "ON 10 (ADC0 > 30)\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        int r = compile(src);
        CHECK(r == 0, "ON <ms> (cond) compiles");
        if (r==0){
            g_now = 0; g_adc = 0;
            run_ticks(5, 10);
            CHECK(vm()->gvar[0].i == 0, "not fired while condition is false");

            g_adc = 50;                      /* 偽→真 */
            run_ticks(5, 10);
            CHECK(vm()->gvar[0].i == 1, "fired once on the rising edge");

            run_ticks(10, 10);               /* 真のまま維持 */
            CHECK(vm()->gvar[0].i == 1, "does NOT re-fire while it stays true (edge, not level)");
        }
    }

    /* 2) 偽で自動リセット → 再成立で再発火 */
    {
        const char *src =
            "ON 10 (ADC0 > 30)\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0; g_adc = 0;
            run_ticks(3, 10);
            g_adc = 50; run_ticks(3, 10);    /* 1回目 */
            CHECK(vm()->gvar[0].i == 1, "1st rising edge");
            g_adc = 0;  run_ticks(3, 10);    /* 偽に落ちる＝リセット */
            CHECK(vm()->gvar[0].i == 1, "falling edge does not fire");
            g_adc = 50; run_ticks(3, 10);    /* 2回目 */
            CHECK(vm()->gvar[0].i == 2, "re-fires after reset (false -> true again)");
            g_adc = 0;  run_ticks(3, 10);
            g_adc = 50; run_ticks(3, 10);    /* 3回目 */
            CHECK(vm()->gvar[0].i == 3, "3rd rising edge");
        }
    }

    /* 3) t0 直後: 最初の満期で既に真なら発火する（prev は load 時 false） */
    {
        const char *src =
            "ON 10 (ADC0 > 30)\n"
            "    1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0; g_adc = 99;           /* 起動時から既に真 */
            run_ticks(4, 10);
            CHECK(vm()->gvar[0].i == 1, "fires at the first poll after t0 when already true");
        }
    }

    /* 4) INIT ではハンドラが発火しない（既存の言語仕様＝橋が閉じている） */
    {
        const char *src =
            "ON 10 (ADC0 > 30)\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "INIT\n"
            "    500 -> WAIT\n"              /* INIT が長く居座る間は発火しない */
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0; g_adc = 99;
            run_ticks(3, 10);                /* まだ INIT フェーズ（WAIT中） */
            CHECK(vm()->gvar[0].i == 0, "no fire during INIT phase (world frozen)");
            run_ticks(60, 10);               /* INIT 抜け → RUN → 発火 */
            CHECK(vm()->gvar[0].i == 1, "fires once after leaving INIT");
        }
    }

    /* 5) 周期がポーリング粒度: 条件が周期の隙間で上下すると取りこぼす（仕様） */
    {
        const char *src =
            "ON 100 (ADC0 > 30)\n"           /* 100ms 粒度 */
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0; g_adc = 0;
            run_ticks(2, 10);
            /* 100ms の満期の合間に 真→偽 と往復させる（10ms tick で 4 tick 分） */
            g_adc = 50; script_tick(); g_now += 10;
            g_adc = 0;  script_tick(); g_now += 10;
            run_ticks(4, 10);
            CHECK(vm()->gvar[0].i == 0, "pulse shorter than the period is missed (documented)");
        }
    }

    /* 6) 純粋式いろいろ: スロット式・論理演算・ビット演算 */
    {
        const char *src =
            "def_handler(SET)\n"
            "ON 10 ((GVAR[1] == 1) AND (ADC0 > 5))\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        int r = compile(src);
        CHECK(r == 0, "slot expr + AND in condition compiles");
        if (r==0){
            g_now = 0; g_adc = 0;
            run_ticks(3, 10);
            vm()->gvar[1] = val_int(1);      /* 片方だけ真 → まだ */
            run_ticks(3, 10);
            CHECK(vm()->gvar[0].i == 0, "AND: not fired with only one operand true");
            g_adc = 9;                       /* 両方真 → 発火 */
            run_ticks(3, 10);
            CHECK(vm()->gvar[0].i == 1, "AND: fires when both become true");
        }
    }

    /* 7) 通常の周期ON（条件なし）は従来どおり毎周期発火（無回帰） */
    {
        const char *src =
            "ON 10\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0;
            run_ticks(6, 10);
            CHECK(vm()->gvar[0].i >= 3, "plain periodic ON still fires every period");
        }
    }

    /* 7b) 自分のトリガ条件を本体が落とすパターン（フラグ消費型・STATUS エラーハンドラ）。
     *     `ON <ms> (STATUS & ERR_x)` … `ERR_x -> CLEAR_ERR` は、clear がそのまま再武装になる。
     *     重要: 「clear した直後・次の満期より前」に条件が再成立しても固まらないこと。
     *     （prev に本体実行“前”の値を入れる実装だと、ここで永久に発火しなくなる回帰があった） */
    {
        const char *src =
            "ON 10\n"
            "    1 / 0 -> GVAR[1]\n"           /* 毎満期 ERR_DIVZERO を立て続ける（バースト） */
            "END\n"
            "ON 100 (STATUS & ERR_DIVZERO)\n"  /* 遅いポーリング＝満期の合間に必ず再成立する */
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "    ERR_DIVZERO -> CLEAR_ERR\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        int r = compile(src);
        CHECK(r == 0, "self-clearing flag handler compiles");
        if (r==0){
            g_now = 0;
            script_clear_status(ERR_DIVZERO);
            run_ticks(60, 10);                 /* 600ms ＝ 100ms ポーリングで約6回ぶん */
            CHECK(vm()->gvar[0].i >= 4,
                  "self-clearing handler keeps firing under burst (not stuck after the 1st)");
        }
    }

    /* 7c) 逆に、本体が条件を変えない場合はエッジのまま（再評価で二重発火しないこと） */
    {
        const char *src =
            "ON 10 (ADC0 > 30)\n"
            "    GVAR[0] + 1 -> GVAR[0]\n"
            "END\n"
            "MAIN\n    1000000 -> WAIT\nEND\n";
        if (compile(src)==0){
            g_now = 0; g_adc = 0;
            run_ticks(3, 10);
            g_adc = 50;
            run_ticks(10, 10);                 /* 真のまま維持 */
            CHECK(vm()->gvar[0].i == 1, "no double-fire when the body does not touch the condition");
        }
    }

    /* 8) 条件が str → ERR_TYPE_MISMATCH */
    {
        int r = compile("ON 10 (\"x\")\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_TYPE_MISMATCH, "string condition -> ERR_TYPE_MISMATCH");
    }

    /* 9) インターバル無しの ON (条件) は作らない → 構文エラー */
    {
        int r = compile("ON (ADC0 > 30)\n    1 -> GVAR[0]\nEND\n");
        CHECK(r != 0, "ON (cond) without an interval is rejected");
    }

    printf("\n%s (fails=%d)\n", g_fail==0 ? "ALL PASS" : "SOME FAILED", g_fail);
    return g_fail ? 1 : 0;
}
