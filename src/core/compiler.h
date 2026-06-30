/* compiler.h - トークナイザ＋ワンパス・コンパイラ（仕様 §2, §13, §15 フェーズ2）
 * ホスト非依存（純C）コア層。 */
#ifndef COMPILER_H
#define COMPILER_H

#include <stddef.h>
#include "script.h"   /* script_err_t / script_error_t */

/* スクリプト全文をコンパイルしVMへ差し替える。0=ok / 非0=script_err_t コード（v0.3.7） */
int compiler_compile(const char *src, size_t len);

/* 直近のロード失敗の構造化詳細（行/コード/aux/tok, §11） */
const script_error_t *compiler_last_error(void);

#endif /* COMPILER_H */
