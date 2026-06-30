/* script.h - ホスト統合API（仕様 §11）
 *
 * 既存FWにソース統合して使う公開インターフェイス。これがホストとコアの境界。
 * 実機ではホストのメインループが下記を呼ぶ:
 *     script_init(arena, size);
 *     // register_* で各ポートをCリソースに束縛（def_* マクロの実体）
 *     script_load(src, len);     // 受信→コンパイル→差し替え
 *     while (1) { ...; script_tick(); }
 *
 * 設計メモ（仕様との差分・確認済み）:
 *   - §11のargvは int32_t* だが、my_print_f が char/int/文字列を出し分けるには
 *     型タグが必要（§1, §10）。よって argv を型タグ付き script_value_t に拡張した。
 *   - §11に無い script_register_inout を追加（§3 def_inout 読み書き両対応のため）。
 *   - スクリプト本文中の def_* 行はC側マクロの写し（§13）として扱い、
 *     コンパイラは読み飛ばす。実際のポート束縛はホストの register_* で行う。
 */
#ifndef SCRIPT_H
#define SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include "value.h"
#include "script_config.h"   /* CFG_MAX_NAME（エラー tok 長に兼用, §12） */

/* Yajir 言語/実装バージョン。スクリプトからは入力ポート VERSION（str産出）で、
 * ホストからは script_version() で読める（§11, v0.4）。 */
#define SCRIPT_VERSION "0.4.0"

/* 出力ポートが受け取る値（型タグ付き）。SV_CHARは文字、SV_INTは数値、SV_STRは
 * script_str()で文字列に解決して出力する。 */
typedef value_t script_value_t;

typedef void    (*script_out_fn)(int argc, const script_value_t *argv);
typedef int32_t (*script_in_fn)(void);

/* ポートの産出型（§3, §11, v0.3.8）。in/inout は登録時に必須指定（out/handler は産出none）。
 * int産出は RESULT 経由、str産出は SRESULT 経由（script_set_result / script_set_sresult）。 */
typedef enum { SCRIPT_T_INT = 0, SCRIPT_T_STR } script_type_t;

/* --- 初期化: ヒープ不使用、固定アリーナを渡す（§11, §12） --- */
void script_init(void *arena, size_t arena_size);

/* 実装バージョン文字列（= SCRIPT_VERSION）。スクリプトの VERSION ポートと同じ値（§11, v0.4） */
const char *script_version(void);

/* --- コンパイル時バインド（INIT前/ load前に呼ぶ, §11） --- */
void script_register_const(const char *name, int32_t value);
/* out: 右辺のみ・産出none・終端（戻り値を持たない）。RESULT/SRESULT を触らない（§3, v0.3.8）。 */
void script_register_out  (const char *name, script_out_fn fn);
/* in: 左辺のみ・産出型必須（int は戻り値、str は fn 内で script_set_sresult、戻り値は無視）。 */
void script_register_in   (const char *name, script_in_fn fn, script_type_t out_type);
/* inout: 両辺OK・産出型必須。set_fn が副作用＋産出（int=script_set_result / str=script_set_sresult）。
 * 物理inout(LED1等)は get_fn で読める。関数ポート(FORMATTER等)は get_fn=NULL（読み不可・送信専用）。 */
void script_register_inout(const char *name, script_in_fn get_fn, script_out_fn set_fn, script_type_t out_type);
void script_register_handler(const char *name);   /* ハンドラ源・産出none（§3, §10） */

/* --- 関数ポートthunkから戻り値をRESULTへ（§4, §11） --- */
void script_set_result(int32_t v);

/* 文字列プールのオフセット→C文字列（出力ポートがSTR値を出すときに使う） */
const char *script_str(int32_t offset);

/* 値が文字列タグか（リテラル or 文字列スロット参照, §4） */
int         script_val_is_str(script_value_t v);
/* 文字列値（SV_STR/SV_SREF）→C文字列。出力ポートやUtilityが使う */
const char *script_resolve_str(script_value_t v);
/* 文字列を返すUtilityの戻り先 SRESULT へ書く（切詰＋ERR_STR_TRUNC, §3,§4） */
void        script_set_sresult(const char *s);

/* --- 非同期源 → VMへの橋（ISRから呼べる。積んで即return, §10） --- */
int script_post_msg     (const char *name, int32_t value); /* int値→ARG[0]      */
int script_post_msg_char(const char *name, char    ch);    /* char型タグ→ARG[0] */
/* 戻り値: 0=ok / <0=キュー満杯（オーバーフローフラグも立つ） */

/* マルチ引数 post（v0.3.5, §11）。混在・複数引数を1イベントとしてアトミックに積む。
 * str引数は post時に SARG バッファへコピー（超過は切詰＋ERR_STR_TRUNC）。
 * 位置 k は ARG[k]（int/char view）/ SARG[k]（str view）で型振り分けして読む。 */
typedef enum { SCRIPT_ARG_T_INT = 0, SCRIPT_ARG_T_CHAR = 1, SCRIPT_ARG_T_STR = 2 } script_argtype_t;
typedef struct { script_argtype_t type; int32_t i; const char *s; int len; } script_arg_t;
#define SCRIPT_ARG_INT(v)     script_arg_make_int((int32_t)(v))
#define SCRIPT_ARG_CHAR(c)    script_arg_make_char((char)(c))
#define SCRIPT_ARG_STR(p, l)  script_arg_make_str((p), (int)(l))   /* 明示長（§11, v0.3.8）。非終端でも可 */
static inline script_arg_t script_arg_make_int (int32_t v)    { script_arg_t a; a.type=SCRIPT_ARG_T_INT;  a.i=v; a.s=0; a.len=0; return a; }
static inline script_arg_t script_arg_make_char(char c)       { script_arg_t a; a.type=SCRIPT_ARG_T_CHAR; a.i=(unsigned char)c; a.s=0; a.len=0; return a; }
static inline script_arg_t script_arg_make_str (const char *s, int len){ script_arg_t a; a.type=SCRIPT_ARG_T_STR; a.i=0; a.s=s; a.len=len; return a; }

/* 引数順は §11 確定形（name, argc, argv）。str引数は len バイトを SARG へコピー（超過は切詰＋ERR_STR_TRUNC）。 */
int script_post_msg_v(const char *name, int argc, const script_arg_t *argv);

/* --- ロードエラー（構造化・(B)方針, v0.3.7 §11）---
 * 文字列化・表示・言語・did-you-mean は「組み込む人の領域」。コアは失敗の事実＋判断材料
 * （行・コード・補助・問題トークン）だけを返す。最初の1エラーで停止（ワンパス・回復なし）。
 * 実行時の STATUS 異常（ERR_QUEUE_OVF 等, §12）とは完全に別系統。 */
/* 注: 名前空間が STATUS ビット（ERR_QUEUE_OVF 等）と ERR_ で被るが、名称は重複せず
 * C上の衝突は無い。仕様 §11 の表記に合わせる。 */
typedef enum {
    ERR_NONE = 0,
    ERR_END_EXPECTED,    /* ブロックが END で閉じてない。aux=開きヘッダの行 */
    ERR_UNEXPECTED_END,  /* 対応ヘッダの無い END */
    ERR_UNKNOWN_PORT,    /* 未登録の送り先。tok=その名前 */
    ERR_UNKNOWN_NAME,    /* 未登録の入力ポート/const/源。tok=その名前 */
    ERR_TYPE_MISMATCH,   /* int/str スロットの型違い代入（§4） */
    ERR_WAIT_IN_ON,      /* ON ハンドラ内の WAIT（§7） */
    ERR_BAD_SLOT_INDEX,  /* 添字が定数でない/範囲外（§4, §12） */
    ERR_NEST_TOO_DEEP,   /* ネスト上限超過（§12） */
    ERR_BAD_POSITION,    /* ポート向き違反: in を右辺 / out を左辺 / 産出noneを中間（§3, v0.3.8） */
    ERR_SYNTAX           /* 上記に当てはまらない構文崩れ（受け皿） */
} script_err_t;

typedef struct {
    int16_t      line;              /* 1始まり。0=行特定不可（全体） */
    script_err_t code;
    int16_t      aux;              /* 補助。LERR_END_EXPECTED の開きヘッダ行 等。無ければ0 */
    char         tok[CFG_MAX_NAME]; /* 問題トークン断片（"HOSTCMP" 等）。無ければ空文字 */
} script_error_t;

/* --- スクリプト全文を受信→コンパイル＆差し替え。RAM常駐・揮発（§11） --- */
int script_load(const char *src, size_t len);   /* 0=ok / 非0=script_err_t コード */
const script_error_t *script_last_error(void);  /* 直近ロード失敗の構造化詳細 */

/* 任意リンクのデバッグ補助（コアに実体は無い・ホスト側で提供）。code→英短文。 */
const char *script_strerror(script_err_t code);

/* --- 1ステップ進める（メインループから周期呼び出し）。タイマ満期もここで処理（§11） --- */
void script_tick(void);

/* --- STATUS 異常フラグ（§12）。スクリプトの STATUS 入力ポートと同じビット集合 --- */
#define ERR_QUEUE_OVF  0x01   /* イベントキュー溢れ */
#define ERR_TIMER_FULL 0x02   /* タイマスロット満杯 */
#define ERR_DIVZERO    0x04   /* 0除算・0剰余 */
#define ERR_STR_TRUNC  0x08   /* 文字列バッファ切り詰め */

int32_t script_get_status(void);            /* 現在の異常フラグ集合 */
void    script_clear_status(int32_t bits);  /* 指定ビットを明示クリア */

/* --- 観測用（オーバーフローフラグ等。デバッグ/テスト用補助） --- */
int  script_event_overflow(void);
int  script_timer_overflow(void);

#endif /* SCRIPT_H */
