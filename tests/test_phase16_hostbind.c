/* test_phase16_hostbind.c - フェーズ16（NOW/STDOUT のコア昇格）の単体テスト（v0.4.8 §8, §11）
 *
 * v0.4.8 で NOW（内部クロック）と STDOUT（出力ポリシー）をコアへ引き上げた:
 *   - script_register_now()   : クロック関数を直保持し vm_now() を O(1) 化。未登録なら
 *                               ロード時 ERR_NO_CLOCK（サイレント故障の防止）。
 *   - script_register_stdout(): ホストはシンク(const char*)だけ渡す。ループ/タグ判定/
 *                               int→10進/末尾\r\n の整形はコアの th_stdout_core が担う。
 * ここでは (1)クロックガード (2)後方互換の走査 (3)STDOUT整形の一致 を検証する。
 * 各ケースは arena を作り直して独立に初期化する（クロック未登録状態を作るため）。
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

/* モッククロック（O(1)経路の確認用に可変値を返す） */
static int32_t g_now = 1234;
static int32_t now_get(void){ return g_now; }

/* STDOUT シンク：出力を1本のバッファに貯めて内容を検査する */
static char g_out[256];
static int  g_outlen = 0;
static void cap_puts(const char *s){
    int n = (int)strlen(s);
    if (g_outlen + n < (int)sizeof(g_out)) { memcpy(g_out + g_outlen, s, n); g_outlen += n; g_out[g_outlen] = '\0'; }
}

int main(void)
{
    static char arena[sizeof(script_vm_t)+64];
    printf("== Phase16 host binding (NOW / STDOUT core promotion) ==\n");

    /* 1) クロックガード: NOW 未登録だと script_load が ERR_NO_CLOCK で失敗する */
    {
        script_init(arena, sizeof(arena));
        /* わざと NOW を登録しない（STDOUT だけ入れてもガードは効く） */
        script_register_stdout(cap_puts);
        int r = compile("INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == ERR_NO_CLOCK, "no clock -> script_load fails with ERR_NO_CLOCK");
        CHECK(script_last_error()->code == ERR_NO_CLOCK, "last_error reports ERR_NO_CLOCK");
    }

    /* 2) script_register_now で登録すれば通り、vm_now() がその値を返す（O(1)経路） */
    {
        script_init(arena, sizeof(arena));
        script_register_now(now_get);
        g_now = 4242;
        CHECK(vm_now() == 4242, "vm_now() returns the registered clock (O(1) path)");
        int r = compile("INIT\n    NOW -> GVAR[0]\nEND\n");
        CHECK(r == 0, "with clock registered, load succeeds");
        if (r == 0) { script_tick(); CHECK(vm()->gvar[0].i == 4242, "NOW readable from script equals the clock"); }
    }

    /* 3) 後方互換: 旧APIの script_register_in("NOW",…) だけでもガードを通過する
     *    （vm_now() が初回走査で now_fn にキャッシュする） */
    {
        script_init(arena, sizeof(arena));
        script_register_in("NOW", now_get, SCRIPT_T_INT);   /* v0.4.7 以前の書き方 */
        int r = compile("INIT\n    1 -> GVAR[0]\nEND\n");
        CHECK(r == 0, "legacy register_in(\"NOW\") still satisfies the clock guard");
        g_now = 77;
        CHECK(vm_now() == 77, "vm_now() resolves & caches the legacy NOW port");
    }

    /* 4) STDOUT 整形はコアが持つ: int→10進 / str そのまま / 混在 / 末尾 \r\n を1回 */
    {
        script_init(arena, sizeof(arena));
        script_register_now(now_get);
        script_register_stdout(cap_puts);
        g_outlen = 0; g_out[0] = '\0';
        int r = compile("INIT\n    \"x=\", 42, \" y=\", -7 -> STDOUT\nEND\n");
        CHECK(r == 0, "STDOUT script compiles");
        if (r == 0) {
            script_tick();
            CHECK(strcmp(g_out, "x=42 y=-7\r\n") == 0, "core formats mixed int/str with trailing CRLF");
        }
    }

    /* 5) STDOUT は任意: 未登録なら -> STDOUT はコンパイル時 ERR_UNKNOWN_PORT */
    {
        script_init(arena, sizeof(arena));
        script_register_now(now_get);   /* クロックはあるが STDOUT は入れない */
        int r = compile("INIT\n    1 -> STDOUT\nEND\n");
        CHECK(r == ERR_UNKNOWN_PORT, "STDOUT is optional: unregistered -> ERR_UNKNOWN_PORT");
    }

    printf(g_fail ? "PHASE16 FAILED (failures=%d)\n" : "PHASE16 PASSED (failures=%d)\n", g_fail);
    return g_fail ? 1 : 0;
}
