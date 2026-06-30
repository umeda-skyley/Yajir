/* host_diag.h - プラットフォーム共通のホスト診断補助（PC / STM32 で共有）
 *
 * コアは「構造化ロードエラー（script_err_t）」しか返さない方針（§11, (B)案）。
 * 表示文の組み立てと did-you-mean（綴り間違い推定）は "組み込む人" の領域なので、
 * ここに純Cで1つだけ実装し、各プラットフォームのホストから使い回す。
 *
 * 依存は script.h のみ（純C・OS非依存）。実機でFlash/RAMが惜しければ、
 * did-you-mean だけデバッグビルド限定にしてもよい。
 */
#ifndef HOST_DIAG_H
#define HOST_DIAG_H

#include "script.h"   /* script_err_t, CFG_MAX_NAME */

/* ロードエラーコード → 人間可読な英語メッセージ。 */
const char *script_strerror(script_err_t code);

/* did-you-mean 候補の収集。host_register_all() が register と一緒に呼ぶ想定。
 *   - host_diag_reset(): 候補リストをクリア（再ロード前に1回）
 *   - host_diag_note(name): 登録したポート名を1つ控える（reg_* ラッパから） */
void host_diag_reset(void);
void host_diag_note(const char *name);

/* 未登録トークン tok に最も近い「登録名 or 組込み予約名」を返す。
 * 十分近くなければ NULL（的外れな提案を避ける）。ロードエラー表示の補助。 */
const char *host_suggest_name(const char *tok);

#endif /* HOST_DIAG_H */
