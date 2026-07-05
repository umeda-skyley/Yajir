/* mathutil.c - 数値ユーティリティの実装（仕様 §3, §9, v0.4.3）
 *
 * 「変換はポート」に則り、乱数・飽和・線形写像・最小/最大/絶対値を int産出ポートで提供する。
 * PRNG は libc rand() に移譲せず自前の xorshift32（状態はアリーナ内 vm()->rng_state・
 * 再現的・シード可能・full 32bit）。※暗号用途ではない（ジッタ/バックオフ/ディザ/ゲーム向け）。
 *
 * ホスト非依存（純C）コア層。
 */
#include <stdint.h>
#include "mathutil.h"
#include "script.h"
#include "vm.h"

/* ---- 乱数（xorshift32） ---- */
/* 状態0は縮退（以後ずっと0）ゆえ、0を踏んだら黄金比定数を既定シードとして採用する。
 * これで「アリーナ0クリア直後」も「0 -> SEED」も安全に非0へ落ちる（縮退回避）。 */
#define RNG_DEFAULT_SEED 0x9E3779B9u

static uint32_t rng_step(void)
{
    uint32_t x = vm()->rng_state;
    if (x == 0) x = RNG_DEFAULT_SEED;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    vm()->rng_state = x;
    return x;
}

/* RAND: 入力ポート（値源, in）。非負の擬似乱数 0..0x7FFFFFFF を1つ産む（§3, v0.4.3）。
 * 範囲は既存の % で作る（例: RAND % 100 -> VAR[0]）。符号ビットを落として常に非負＝
 * RAND % N が負にならないことを保証する。 */
static int32_t rand_get(void)
{
    return (int32_t)(rng_step() & 0x7FFFFFFFu);
}

/* SEED: out ポート（産出none）。PRNG を再シードする（例: 12345 -> SEED / NOW -> SEED）。
 * 値0は rng_step 側で既定シードへ落ちる（縮退回避）。 */
static void seed_set(int argc, const script_value_t *a)
{
    vm()->rng_state = (argc > 0) ? (uint32_t)a[0].i : 0u;
}

/* ホストからのエントロピー注入（起動時に ADCノイズ/UID/tick 等で・§11, v0.4.3）。
 * 0 を渡しても rng_step で既定シードへ落ちるので安全。 */
void script_srand(uint32_t seed)
{
    vm()->rng_state = seed;
}

/* ---- 飽和・写像・縮約（すべて int産出＝RESULT） ---- */

/* CLAMP: x, lo, hi -> RESULT。x を [lo, hi] に収める（min(max(x,lo),hi)）。
 * 引数不足は0埋め（ルーズ, §5）。lo>hi の場合は hi に張り付く（benign）。 */
static void th_clamp(int argc, const script_value_t *a)
{
    int32_t x  = (argc > 0) ? a[0].i : 0;
    int32_t lo = (argc > 1) ? a[1].i : 0;
    int32_t hi = (argc > 2) ? a[2].i : 0;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    script_set_result(x);
}

/* MAP: x, inLo, inHi, outLo, outHi -> RESULT。入力レンジ内の x の位置を出力レンジへ
 * 線形写像する（Arduino map 相当）。整数割り算で切り捨て・入力レンジ外は外挿（クランプしない）。
 * inHi==inLo はゼロ除算 → ERR_DIVZERO を立てて RESULT=0（既存の /0 継続方針に一致・§9）。
 * 中間積は int64 で計算しオーバーフローを回避（最終結果は int32）。 */
static void th_map(int argc, const script_value_t *a)
{
    int32_t x  = (argc > 0) ? a[0].i : 0;
    int32_t il = (argc > 1) ? a[1].i : 0;
    int32_t ih = (argc > 2) ? a[2].i : 0;
    int32_t ol = (argc > 3) ? a[3].i : 0;
    int32_t oh = (argc > 4) ? a[4].i : 0;
    int64_t num;
    if (ih == il) { vm_set_err(ERR_DIVZERO); script_set_result(0); return; }
    num = (int64_t)(x - il) * (int64_t)(oh - ol);
    script_set_result((int32_t)((int64_t)ol + num / (int64_t)(ih - il)));
}

/* MIN/MAX: a, b, … -> RESULT。可変長・全引数の最小/最大（引数0なら0）。 */
static void th_min(int argc, const script_value_t *a)
{
    int32_t r; int i;
    if (argc < 1) { script_set_result(0); return; }
    r = a[0].i;
    for (i = 1; i < argc; i++) if (a[i].i < r) r = a[i].i;
    script_set_result(r);
}
static void th_max(int argc, const script_value_t *a)
{
    int32_t r; int i;
    if (argc < 1) { script_set_result(0); return; }
    r = a[0].i;
    for (i = 1; i < argc; i++) if (a[i].i > r) r = a[i].i;
    script_set_result(r);
}

/* ABS: x -> RESULT。絶対値。INT32_MIN は反転不能（UB）ゆえ INT32_MAX に飽和させる。 */
static void th_abs(int argc, const script_value_t *a)
{
    int32_t x = (argc > 0) ? a[0].i : 0;
    if (x == (int32_t)0x80000000) x = 0x7FFFFFFF;
    else if (x < 0)               x = -x;
    script_set_result(x);
}

void register_mathutils(void)
{
    script_register_in   ("RAND",  rand_get, SCRIPT_T_INT);      /* 非負乱数源 0..2^31-1（§3, v0.4.3） */
    script_register_out  ("SEED",  seed_set);                    /* 再シード（値 -> SEED） */
    script_register_inout("CLAMP", NULL, th_clamp, SCRIPT_T_INT);/* x を [lo,hi] に飽和 */
    script_register_inout("MAP",   NULL, th_map,   SCRIPT_T_INT);/* 線形リスケール（Arduino map） */
    script_register_inout("MIN",   NULL, th_min,   SCRIPT_T_INT);/* 可変長・最小 */
    script_register_inout("MAX",   NULL, th_max,   SCRIPT_T_INT);/* 可変長・最大 */
    script_register_inout("ABS",   NULL, th_abs,   SCRIPT_T_INT);/* 絶対値 */
}
