/* value.h - tagged value（仕様 §9, §4 RESULT/ARG型タグ）
 *
 * VMが扱う唯一の値表現。int32基準。真偽はintで代用（0=偽/非0=真）。
 * 型タグは「出力先（my_print_f等）が文字/数値/文字列を出し分ける」ために保持する（§1, §10）。
 *   - VT_INT  : 整数。.i に値。
 *   - VT_CHAR : 文字タグ付き整数。.i に文字コード。my_print_fは文字として出す。
 *   - VT_STR  : 文字列定数参照。.i は文字列プール内オフセット（script_str()で解決）。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef VALUE_H
#define VALUE_H

#include <stdint.h>

/* タグ名は Windows の wtypes.h（VARIANT用 VT_INT 等）と衝突するため SV_ 接頭辞。 */
typedef enum {
    SV_INT  = 0,
    SV_CHAR = 1,
    SV_STR  = 2,   /* 文字列リテラル参照：i = 文字列プールオフセット（読み取り専用） */
    SV_SREF = 3    /* 文字列スロット参照：i = (kind<<8)|idx（§4 文字列スロット） */
} value_type_t;

/* SV_SREF の kind（文字列スロット種別）。SARG は受信専用の読みビュー（§4, §10）。 */
enum { SSLOT_SVAR = 0, SSLOT_SGVAR = 1, SSLOT_SRESULT = 2, SSLOT_SARG = 3 };

typedef struct {
    value_type_t tag;
    int32_t      i;   /* INT:値 / CHAR:文字コード / STR:プールoff / SREF:(kind<<8)|idx */
} value_t;

static inline value_t val_int(int32_t v)  { value_t x; x.tag = SV_INT;  x.i = v; return x; }
static inline value_t val_char(int32_t c) { value_t x; x.tag = SV_CHAR; x.i = c; return x; }
static inline value_t val_str(int32_t o)  { value_t x; x.tag = SV_STR;  x.i = o; return x; }
static inline value_t val_bool(int b)     { value_t x; x.tag = SV_INT;  x.i = b ? 1 : 0; return x; }
static inline value_t val_sref(int kind, int idx){ value_t x; x.tag = SV_SREF; x.i = (kind<<8)|idx; return x; }

/* 文字列タグか（リテラル or スロット参照） */
static inline int val_is_str(value_t v) { return v.tag == SV_STR || v.tag == SV_SREF; }

/* 真偽判定：0=偽 / 非0=真（タグ無関係に .i を見る） */
static inline int val_truthy(value_t v)   { return v.i != 0; }

#endif /* VALUE_H */
