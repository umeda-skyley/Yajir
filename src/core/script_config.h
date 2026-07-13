/* script_config.h - コンパイル時定数（仕様 §12）
 *
 * ターゲットRAMに合わせて確定する静的見積もりパラメータ。
 * ここを変えるだけで各テーブル/スタック/バッファのサイズが決まる。
 * ホスト非依存（純C）コア層の一部。
 */
#ifndef SCRIPT_CONFIG_H
#define SCRIPT_CONFIG_H

/* Platform/build-specific overrides.
 *
 * Define YJ_CONFIG_HEADER as a quoted header name from the build system, e.g.
 *   /DYJ_CONFIG_HEADER=\"yajir_config_pc.h\"
 * or
 *   -DYJ_CONFIG_HEADER=\"board_config.h\"
 * The included header may #define any CFG_* value before the defaults below.
 */
#ifdef YJ_CONFIG_HEADER
#include YJ_CONFIG_HEADER
#endif

/* 固定スロット（§4） */
#ifndef CFG_GVAR_COUNT
#define CFG_GVAR_COUNT      8     /* GVAR[] 要素数（永続） */
#endif
#ifndef CFG_VAR_COUNT
#define CFG_VAR_COUNT       8     /* VAR[]  要素数（揮発） */
#endif
#ifndef CFG_ARG_COUNT
#define CFG_ARG_COUNT       4     /* ARG[]  最大数（受信引数） */
#endif

/* 文字列スロット（§4 文字列スロット・案A） */
#ifndef CFG_SGVAR_COUNT
#define CFG_SGVAR_COUNT     4     /* SGVAR[] 本数（永続・文字列） */
#endif
#ifndef CFG_SVAR_COUNT
#define CFG_SVAR_COUNT      4     /* SVAR[]  本数（揮発・文字列） */
#endif
#ifndef CFG_SSTR_LEN
#define CFG_SSTR_LEN        64    /* 文字列スロット1本のバッファ長（終端含む） */
#endif

/* 受信文字列引数スロット（§4, §10, v0.3.5）。ARG[k]/SARG[k] は同位置の型2ビュー。
 * 文字列は先頭 CFG_SARG_COUNT 位置にだけ置ける（SARG[k] は位置 k と共有）。
 * 例: (int, int, str) のように位置2に文字列を置くなら 3 以上が必要。 */
#ifndef CFG_SARG_COUNT
#define CFG_SARG_COUNT      4     /* SARG[] 本数（= str 引数を置ける先頭位置数）。位置3まで文字列可 */
#endif
#ifndef CFG_SARG_LEN
#define CFG_SARG_LEN        32    /* SARG 1本の受信文字列バッファ長（終端含む） */
#endif

/* テーブル上限（§12） */
#ifndef CFG_MAX_PORTS
#define CFG_MAX_PORTS       56    /* 最大ポート数（組込み〜29＋ホスト分の余裕。超えると後勝ちで黙ってregister失敗） */
#endif
#ifndef CFG_MAX_BLOCKS
#define CFG_MAX_BLOCKS      16    /* 最大ブロック（INIT/MAIN/ON…）数 */
#endif
#ifndef CFG_MAX_RESOURCES
#define CFG_MAX_RESOURCES   32    /* ホストCリソース登録数（def_*が束縛する先） */
#endif
#ifndef CFG_MAX_ALIAS
#define CFG_MAX_ALIAS       16    /* def_alias の最大数（コンパイル時のみ・名前→スロット/数値/文字列） */
#endif

/* VM 実行資源（§12） */
#ifndef CFG_STACK_DEPTH
#define CFG_STACK_DEPTH     32    /* オペランドスタック深さ */
#endif
#ifndef CFG_NEST_LIMIT
#define CFG_NEST_LIMIT      4     /* IFYESネスト上限（§7：4段静的確保） */
#endif
#ifndef CFG_LOOP_NEST
#define CFG_LOOP_NEST       4     /* REPEATループのネスト上限（§7, v0.4.4） */
#endif
#ifndef CFG_CALL_NEST
#define CFG_CALL_NEST       4     /* スクリプト内ポート呼び出し深さ上限（§3, v0.4.5・ロード時DFSで検算） */
#endif
#ifndef CFG_MAX_SCRIPT_PORTS
#define CFG_MAX_SCRIPT_PORTS 16   /* def_port の最大数（コンパイル時のDAG判定用・ビットセット幅≤32, v0.4.5） */
#endif
#ifndef CFG_INSTR_BUDGET
#define CFG_INSTR_BUDGET    20000 /* 1 tickあたりの命令数バジェット（暴走防止） */
#endif

/* イベント/タイマ（§8, §10） */
#ifndef CFG_EVENT_QUEUE_LEN
#define CFG_EVENT_QUEUE_LEN 16    /* イベントキュー長（固定長・溢れドロップ） */
#endif
#ifndef CFG_TIMER_SLOTS
#define CFG_TIMER_SLOTS     4     /* タイマスロット数（=4目安） */
#endif

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
#ifndef CFG_CODE_SIZE
#define CFG_CODE_SIZE       2048  /* バイトコードバッファ（バイト）。PERサンプルで約400B使用＝2KBで十分 */
#endif
#ifndef CFG_STRPOOL_SIZE
#define CFG_STRPOOL_SIZE    1024  /* 文字列定数プール（バイト） */
#endif

/* 識別子 */
#ifndef CFG_MAX_NAME
#define CFG_MAX_NAME        24    /* ポート名/リソース名の最大長（終端含む） */
#endif

/* 行デリミタ（§2）。端末の改行は CR / CRLF が多い。既定では CR・LF のどちらも行末として
 * 受理し、CRLF は1改行に畳む（CR / LF / CRLF いずれも1論理行・行番号は正確）＝どの端末でも動く。
 * CFG_LINE_DELIM は「行末を表す主デリミタ」（既定 CR）。1文字に厳密化したいときは
 * CFG_LINE_DELIM_STRICT=1 にする（CFG_LINE_DELIM 以外の CR/LF は空白として読み飛ばす）。 */
#ifndef CFG_LINE_DELIM
#define CFG_LINE_DELIM        '\r'
#endif
#ifndef CFG_LINE_DELIM_STRICT
#define CFG_LINE_DELIM_STRICT 0
#endif

/* イベントキュー enqueue のクリティカルセクション（ISR直post・MPSC安全, §10/§11 v0.4.1）。
 *
 * 生ISRから script_post_msg* を直接呼べるように、enqueue（evq_push の tail 公開）だけを
 * 短いクリティカルセクションで囲う。割り込み禁止/復帰の作法はターゲット依存なので、
 * コアは下の2マクロ越しに囲むだけ＝コア自体はホスト非依存（純C）を保つ。
 *
 *   - 既定は空（何もしない）。生産者が実質1つのホスト（PCモック等）はこのままでコスト0。
 *   - 実機（複数生産者＝ISR＋self-post）では、ホストが自ターゲット向けに再定義する。
 *     差し込み方は2通り: (a) ビルド時に -DYJ_PORT_HEADER="\"your_port.h\"" で自前ヘッダを取り込む、
 *     (b) ビルド前に YJ_ENTER_CRITICAL / YJ_EXIT_CRITICAL を直接 #define しておく。
 *   例（Cortex-M / CMSIS・PRIMASK退避で再入安全）:
 *     #define YJ_ENTER_CRITICAL()  uint32_t _yj_pm = __get_PRIMASK(); __disable_irq()
 *     #define YJ_EXIT_CRITICAL()   __set_PRIMASK(_yj_pm)
 *   ※ENTER が保存変数を宣言し EXIT がそれを使う形なので、両者は同一ブロック内で対で使うこと。 */
#ifdef YJ_PORT_HEADER
#include YJ_PORT_HEADER
#endif
#ifndef YJ_ENTER_CRITICAL
#define YJ_ENTER_CRITICAL()   ((void)0)
#endif
#ifndef YJ_EXIT_CRITICAL
#define YJ_EXIT_CRITICAL()    ((void)0)
#endif

#endif /* SCRIPT_CONFIG_H */
