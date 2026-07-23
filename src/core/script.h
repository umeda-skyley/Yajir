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
#define SCRIPT_VERSION "0.4.11"

/* 出力ポートが受け取る値（型タグ付き）。SV_INTは数値、SV_STRは script_str()で文字列に
 * 解決して出力する（v0.4.2でCHARタグ撤去＝値は int/str の2択・§9。数を文字グリフで出すのは
 * 出力側の変換 CHR / FORMATTER %c の仕事）。 */
typedef value_t script_value_t;

typedef void    (*script_out_fn)(int argc, const script_value_t *argv);
typedef int32_t (*script_in_fn)(void);
/* STDOUT のシンク（1文字列を出力）。コアが出力ポリシー（ループ/タグ判定/int→10進/改行）を
 * 所有し、ホストはこの関数＝出力先だけを渡す（§11, v0.4.8）。 */
typedef void    (*script_puts_fn)(const char *s);
/* def_import("NAME") が呼ぶライブラリ取得（§13, v0.4.10）。ライブラリの所在はホスト依存
 * （Flash常駐の const char[] 表／ファイル／フラッシュFS…）なので、コアは構文だけ持ち、
 * 実体はこの1本の注入で受け取る。本文は「コンパイル中だけ」有効ならよい（コアは原文を保持しない）。
 *   戻り値: 0=見つかった（*src/*len に本文）／<0=その名前は無い（ERR_IMPORT_NOT_FOUND）
 * 未登録（＝この関数を渡さない）＝importをサポートしないプラットフォーム（ERR_NO_IMPORT）。 */
typedef int     (*script_import_fn)(const char *name, const char **src, uint32_t *len);

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
/* NOW: 単調増加クロック(ms)を登録する。内部クロック（タイマ/周期/WAIT/遅延post の源）＝コア必須（§8, v0.4.8）。
 * PK_IN "NOW" も同時に登録するので、スクリプトからは NOW -> x で読める。
 * 未登録のまま script_load すると ERR_NO_CLOCK で失敗する（サイレント故障の防止）。 */
void script_register_now(script_in_fn tick);
/* STDOUT: 出力シンクだけを渡す。ループ/タグ判定/int→10進/改行のポリシーはコアが持つ（§11, v0.4.8）。
 * 任意（未登録なら STDOUT ポートは無く、-> STDOUT はコンパイル時 ERR_UNKNOWN_PORT）。 */
void script_register_stdout(script_puts_fn puts_fn);
/* def_import: ライブラリ取得関数を渡す（§13, v0.4.10）。任意＝未登録ならスクリプト中の
 * def_import は ERR_NO_IMPORT でロード失敗する（黙って無視はしない）。 */
void script_register_import(script_import_fn fn);
/* script: スクリプト内ポート（def_port）。本体は PORT ブロック（コンパイラが bc_start を後埋め）。
 * inout 同格・産出型必須・両辺可でチェイン可（§3, §7, v0.4.5）。 */
void script_register_script_port(const char *name, script_type_t out_type);

/* --- 関数ポートthunkから戻り値をRESULTへ（§4, §11） --- */
void script_set_result(int32_t v);

/* --- 乱数シード注入（RAND/SEED ポートの PRNG, §3 v0.4.3）---
 * ホストが起動時に ADCノイズ/UID/tick 等のエントロピーで種を入れる。省略時は既定の固定種
 * （再現的）。スクリプト側は `値 -> SEED` で再シードできる。0 を渡しても内部で既定種に落ちる。 */
void script_srand(uint32_t seed);

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
int script_post_msg_char(const char *name, char    ch);    /* = post_msg の互換別名（受信バイトを int で ARG[0]、v0.4.2） */
/* 戻り値: 0=ok / <0=キュー満杯（オーバーフローフラグも立つ） */

/* マルチ引数 post（v0.3.5, §11）。混在・複数引数を1イベントとしてアトミックに積む。
 * str引数は post時に SARG バッファへコピー（超過は切詰＋ERR_STR_TRUNC）。
 * 位置 k は ARG[k]（int view）/ SARG[k]（str view）で型振り分けして読む（CHAR撤去・v0.4.2）。 */
typedef enum { SCRIPT_ARG_T_INT = 0, SCRIPT_ARG_T_STR = 2 } script_argtype_t;  /* CHAR(=1) は撤去（v0.4.2, §9） */
typedef struct { script_argtype_t type; int32_t i; const char *s; int len; } script_arg_t;
#define SCRIPT_ARG_INT(v)     script_arg_make_int((int32_t)(v))
/* v0.4.2: CHAR 撤去により INT の別名（受信バイトは int）。互換のため名前だけ温存 */
#define SCRIPT_ARG_CHAR(c)    script_arg_make_int((int32_t)(unsigned char)(c))
#define SCRIPT_ARG_STR(p, l)  script_arg_make_str((p), (int)(l))   /* 明示長（§11, v0.3.8）。非終端でも可 */
static inline script_arg_t script_arg_make_int (int32_t v)    { script_arg_t a; a.type=SCRIPT_ARG_T_INT;  a.i=v; a.s=0; a.len=0; return a; }
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
    ERR_WAIT_IN_LOOP,    /* REPEAT ループ内の WAIT（§7, v0.4.4。within-tick 維持のため禁止） */
    ERR_BAD_SLOT_INDEX,  /* 添字が定数でない/範囲外（§4, §12） */
    ERR_NEST_TOO_DEEP,   /* ネスト上限超過（§12） */
    ERR_BAD_POSITION,    /* ポート向き違反: in を右辺 / out を左辺 / 産出noneを中間（§3, v0.3.8） */
    ERR_TOO_MANY_PORTS,  /* スクリプト def_handler/def_port でポート表が満杯（§3, v0.4.1/v0.4.5） */
    ERR_RECURSION,       /* スクリプト内ポートの再帰サイクル（§3, v0.4.5・load時DFS）。tok=サイクル上のポート名 */
    ERR_NO_CLOCK,        /* 内部クロック未登録（§8, v0.4.8）。script_register_now を呼び忘れ＝時間が動かない */
    ERR_NO_IMPORT,       /* def_import があるがホストが取得関数を注入していない（§13, v0.4.10）。tok=ライブラリ名 */
    ERR_IMPORT_NOT_FOUND,/* 取得関数はあるがその名前のライブラリが無い（§13, v0.4.10）。tok=ライブラリ名 */
    ERR_DUP_DEF,         /* スクリプト内ポートの二重定義（§3, v0.4.11）。def_port の再宣言 / PORT 本体の
                          * 二重定義 / 既登録名との衝突。tok=その名前。ライブラリと本体の名前衝突で出やすい */
    ERR_SYNTAX           /* 上記に当てはまらない構文崩れ（受け皿） */
} script_err_t;

typedef struct {
    int16_t      line;              /* 1始まり。0=行特定不可（全体） */
    script_err_t code;
    int16_t      aux;              /* 補助。LERR_END_EXPECTED の開きヘッダ行 等。無ければ0 */
    char         tok[CFG_MAX_NAME]; /* 問題トークン断片（"HOSTCMP" 等）。無ければ空文字 */
    /* どのソースの line か（§13, v0.4.10）。空文字＝本体スクリプト、非空＝その名前のライブラリ内。
     * def_import で行番号が2つのソースに分かれるので、表示側はこれを添える。 */
    char         src_name[CFG_MAX_NAME];
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
#define ERR_BUDGET     0x10   /* REPEAT が命令数バジェットで打ち切られた（§7, v0.4.4） */
#define ERR_DELAY_FULL 0x20   /* 遅延post の pending 表が満杯で新着を捨てた（§10, v0.4.7）。
                               * ※満期時にイベントキューが満杯だった場合は既存の ERR_QUEUE_OVF（切り分け可） */

int32_t script_get_status(void);            /* 現在の異常フラグ集合 */
void    script_clear_status(int32_t bits);  /* 指定ビットを明示クリア */

/* --- 観測用（オーバーフローフラグ等。デバッグ/テスト用補助） --- */
int  script_event_overflow(void);
int  script_timer_overflow(void);

#endif /* SCRIPT_H */
