/* script_config.h - コンパイル時定数（仕様 §12）
 *
 * ターゲットRAMに合わせて確定する静的見積もりパラメータ。
 * ここを変えるだけで各テーブル/スタック/バッファのサイズが決まる。
 * ホスト非依存（純C）コア層の一部。
 */
#ifndef SCRIPT_CONFIG_H
#define SCRIPT_CONFIG_H

/* 固定スロット（§4） */
#define CFG_GVAR_COUNT      8     /* GVAR[] 要素数（永続） */
#define CFG_VAR_COUNT       8     /* VAR[]  要素数（揮発） */
#define CFG_ARG_COUNT       4     /* ARG[]  最大数（受信引数） */

/* 文字列スロット（§4 文字列スロット・案A） */
#define CFG_SGVAR_COUNT     4     /* SGVAR[] 本数（永続・文字列） */
#define CFG_SVAR_COUNT      4     /* SVAR[]  本数（揮発・文字列） */
#define CFG_SSTR_LEN        64    /* 文字列スロット1本のバッファ長（終端含む） */

/* 受信文字列引数スロット（§4, §10, v0.3.5）。ARG[k]/SARG[k] は同位置の型2ビュー。
 * 文字列は先頭 CFG_SARG_COUNT 位置にだけ置ける（SARG[k] は位置 k と共有）。
 * 例: (int, int, str) のように位置2に文字列を置くなら 3 以上が必要。 */
#define CFG_SARG_COUNT      4     /* SARG[] 本数（= str 引数を置ける先頭位置数）。位置3まで文字列可 */
#define CFG_SARG_LEN        32    /* SARG 1本の受信文字列バッファ長（終端含む） */

/* テーブル上限（§12） */
#define CFG_MAX_PORTS       48    /* 最大ポート数（組込み21＋ホスト分の余裕。超えると後勝ちで黙ってregister失敗） */
#define CFG_MAX_BLOCKS      16    /* 最大ブロック（INIT/MAIN/ON…）数 */
#define CFG_MAX_RESOURCES   32    /* ホストCリソース登録数（def_*が束縛する先） */
#define CFG_MAX_ALIAS       16    /* def_alias の最大数（コンパイル時のみ・名前→スロット/数値/文字列） */

/* VM 実行資源（§12） */
#define CFG_STACK_DEPTH     32    /* オペランドスタック深さ */
#define CFG_NEST_LIMIT      4     /* IFYESネスト上限（§7：4段静的確保） */
#define CFG_INSTR_BUDGET    20000 /* 1 tickあたりの命令数バジェット（暴走防止） */

/* イベント/タイマ（§8, §10） */
#define CFG_EVENT_QUEUE_LEN 16    /* イベントキュー長（固定長・溢れドロップ） */
#define CFG_TIMER_SLOTS     4     /* タイマスロット数（=4目安） */

/* 周期ON（ON <ms>）が満期を取りこぼしたときの方針（コンパイルオプション, §6）。
 * 仕様の既定は catch-up。ビルド時に /DTS_PERIODIC_CATCHUP=0 を渡せば coalesce に切替。
 *
 *   1 = catch-up（既定）:
 *       パスした回数ぶんハンドラを呼ぶ。next_time は1周期ずつだけ進めるので 1 tick につき
 *       最大1回発火（取り戻しは以降のtickに1回ずつ分散＝同tickバースト回避, §6）。
 *       過負荷が続けば実時刻に対して遅れていく（ドリフトを無理に復帰しない）。回数が資産の用途向け。
 *
 *   0 = coalesce:
 *       何周パスしても実質1回に丸め、次回満期を現在基準（周期グリッド上の次の点）で張り直す。
 *       発火回数は減るが、最新状態だけが意味を持つ用途・取り戻し発火を嫌う場面向け。 */
#ifndef TS_PERIODIC_CATCHUP
#define TS_PERIODIC_CATCHUP 1
#endif

/* コンパイル後のサイズ */
#define CFG_CODE_SIZE       2048  /* バイトコードバッファ（バイト）。PERサンプルで約400B使用＝2KBで十分 */
#define CFG_STRPOOL_SIZE    1024  /* 文字列定数プール（バイト） */

/* 識別子 */
#define CFG_MAX_NAME        24    /* ポート名/リソース名の最大長（終端含む） */

/* 行デリミタ（§2）。端末の改行は CR / CRLF が多い。既定では CR・LF のどちらも行末として
 * 受理し、CRLF は1改行に畳む（CR / LF / CRLF いずれも1論理行・行番号は正確）＝どの端末でも動く。
 * CFG_LINE_DELIM は「行末を表す主デリミタ」（既定 CR）。1文字に厳密化したいときは
 * CFG_LINE_DELIM_STRICT=1 にする（CFG_LINE_DELIM 以外の CR/LF は空白として読み飛ばす）。 */
#define CFG_LINE_DELIM        '\r'
#ifndef CFG_LINE_DELIM_STRICT
#define CFG_LINE_DELIM_STRICT 0
#endif

#endif /* SCRIPT_CONFIG_H */
