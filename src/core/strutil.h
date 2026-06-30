/* strutil.h - 標準装備Utilityポート（仕様 §3 標準装備Utility, §15 フェーズ7）
 *
 * SLICER / MERGER / COUNTER / FORMATTER / EQUALS を組み込みポートとして登録する。
 * 位置は1始まり・範囲は(開始,長さ)・1バイト1文字ASCII前提。
 * 文字列を返すものは SRESULT、数値を返すものは RESULT へ。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef STRUTIL_H
#define STRUTIL_H

void register_strutils(void);   /* register_builtins から呼ぶ（script_init時） */

#endif /* STRUTIL_H */
