/* mathutil.h - 標準装備 数値ユーティリティポート（仕様 §3, v0.4.3）
 *
 * RAND/SEED（自前 xorshift32 PRNG）・CLAMP・MAP・MIN・MAX・ABS を組み込みポートとして登録する。
 * すべて int産出（RESULT 経由）。RAND は値源(in)、SEED は産出none の out。
 * 「変換はポート」に則った strutil の数値版（§3 標準装備Utility）。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef MATHUTIL_H
#define MATHUTIL_H

void register_mathutils(void);   /* register_builtins から呼ぶ（script_init時） */

#endif /* MATHUTIL_H */
