/* vm.h - VM内部状態とバイトコード実行（仕様 §1, §4, §12, §15）
 *
 * 全状態は単一の script_vm_t に集約し、script_init() で渡された
 * 固定アリーナ上に配置する（ヒープ不使用・§12）。
 * スケジューラ/イベントキュー/タイマのフィールドも本構造体に同居し、
 * フェーズ3,4で利用する。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef VM_H
#define VM_H

#include <stddef.h>
#include <stdbool.h>
#include "script_config.h"
#include "value.h"
#include "opcodes.h"
#include "script.h"   /* script_type_t（ポート産出型, v0.3.8） */

/* ---- ポート（§3） ---- */
typedef void    (*out_fn_t)(int argc, const value_t *argv); /* def_out / def_inout write */
typedef int32_t (*in_fn_t)(void);                           /* def_in  / def_inout read  */

typedef enum {
    PK_OUT,    /* def_out   : 関数ポート（戻り値→RESULT）   */
    PK_IN,     /* def_in    : 入力ポート（式中で読む）       */
    PK_INOUT,  /* def_inout : 物理。読み書き両対応           */
    PK_CONST,    /* def_const   : 名前付き整数定数             */
    PK_HANDLER   /* def_handler : ON <NAME> ハンドラ源         */
} port_kind_t;

typedef struct {
    char          name[CFG_MAX_NAME];
    port_kind_t   kind;
    script_type_t out_type;   /* 産出型（PK_IN / PK_INOUT のみ有効, v0.3.8）。out/handler/const は未使用 */
    int32_t       const_val;  /* PK_CONST */
    in_fn_t       get_fn;     /* PK_IN / PK_INOUT（関数ポートは NULL） */
    out_fn_t      set_fn;     /* PK_OUT / PK_INOUT */
} port_t;

/* ---- ブロック/トリガ（§6） ---- */
typedef enum {
    BLK_INIT,
    BLK_MAIN,
    BLK_ON_PERIOD,   /* ON <ms>     周期           */
    BLK_ON_HANDLER,  /* ON <源>     def_handler発火（HANDLER含む名前付きハンドラ） */
    BLK_ON_TIMER     /* ON TIMER    満期           */
} block_kind_t;

typedef struct {
    block_kind_t kind;
    int32_t      period;     /* BLK_ON_PERIOD: 周期ms          */
    int          handler_port; /* BLK_ON_HANDLER: 対応ポート番号  */
    uint16_t     bc_start;   /* バイトコード開始オフセット       */
    int32_t      next_time;  /* BLK_ON_PERIOD: 次回発火予定tick  */
} block_t;

/* ---- イベント（§10, v0.3.5 多値）----
 * 1イベント＝最大 CFG_ARG_COUNT 個の「位置」。各位置は int/char か str。
 * str位置の中身は sstr[k]（先頭 CFG_SARG_COUNT 位置まで）に post時コピー済み。
 * 受信側は ARG[k]（int/charビュー）/ SARG[k]（strビュー）で型振り分けして読む。 */
typedef enum { EVT_HANDLER, EVT_TIMER } event_kind_t;
typedef struct {
    event_kind_t kind;
    int          handler_port;                      /* EVT_HANDLER: 発火源ポート番号 */
    int          npos;                              /* 充填された位置数 */
    value_t      pos[CFG_ARG_COUNT];                /* INT/CHAR=値, STR=tag SV_STR マーカ */
    char         sstr[CFG_SARG_COUNT][CFG_SARG_LEN];/* str位置 k(<N_SARG) の文字列 */
} event_t;

/* ---- タイマスロット（§8） ---- */
typedef struct {
    bool    active;
    int32_t fire_time;   /* この時刻(tick)以降で満期。init_armed時は t0 起点の相対ms（オフセット） */
    bool    init_armed;  /* INITフェーズで張った→締切は t0 起点で解決（v0.3.4, §8） */
} timer_slot_t;

/* ---- yield実行コンテキスト（§7 WAIT yield/再開）。
 * INIT/MAIN で共用（INITフェーズではINIT、RUNフェーズではMAINを保持。両者は同時に走らない）。 */
typedef struct {
    bool     started;     /* 現フェーズの yield ブロックが存在するか */
    uint16_t pc;          /* 次に実行する命令オフセット */
    bool     waiting;     /* WAIT中か */
    int32_t  wake_time;   /* waiting時：この時刻で再開 */
} main_ctx_t;

/* ---- VM本体 ---- */
typedef struct {
    /* オペランドスタック */
    value_t stack[CFG_STACK_DEPTH];
    int     sp;

    /* 固定スロット（§4） */
    value_t gvar[CFG_GVAR_COUNT];
    value_t var[CFG_VAR_COUNT];
    value_t arg[CFG_ARG_COUNT];                 /* ARG[k]：int/char ビュー（§10） */
    char    sarg[CFG_SARG_COUNT][CFG_SARG_LEN]; /* SARG[k]：str ビュー（受信専用, v0.3.5） */
    value_t result;

    /* 文字列スロット（§4 文字列スロット・案A）。固定長バッファ。 */
    char    sgvar[CFG_SGVAR_COUNT][CFG_SSTR_LEN];
    char    svar[CFG_SVAR_COUNT][CFG_SSTR_LEN];
    char    sresult[CFG_SSTR_LEN];

    /* 異常フラグ集合（§12 STATUS）。ERR_xxx ビットの論理和。 */
    int32_t status;

    /* ポート表（§3） */
    port_t  ports[CFG_MAX_PORTS];
    int     nports;

    /* バイトコードと文字列プール */
    uint8_t code[CFG_CODE_SIZE];
    int     code_len;
    char    strpool[CFG_STRPOOL_SIZE];
    int     strpool_len;

    /* ブロック表 */
    block_t blocks[CFG_MAX_BLOCKS];
    int     nblocks;
    int     init_blk;   /* index or -1 */
    int     main_blk;   /* index or -1 */

    /* スケジューラ（§3, §15 フェーズ3） */
    main_ctx_t main_ctx;
    bool       loaded;        /* 実行可能なプログラムがロード済みか */
    bool       init_done;     /* INITを実行済みか */

    /* イベントキュー（SPSCリング, §10） */
    event_t evq[CFG_EVENT_QUEUE_LEN];
    volatile int evq_head;    /* 消費側が進める */
    volatile int evq_tail;    /* 生産側(post_*)が進める。ISRから触る境界 */
    volatile bool evq_overflow;

    /* タイマ（§8） */
    timer_slot_t timers[CFG_TIMER_SLOTS];
    bool         timer_overflow;
} script_vm_t;

/* 単一インスタンスへのアクセス（アリーナ上に配置） */
script_vm_t *vm(void);

/* script_init から：アリーナにVMを配置しゼロ初期化。0=ok / <0=不足 */
int     vm_place_arena(void *arena, size_t size);
/* スケジューラ/タイマ用の単調クロック（NOWポート経由, §3） */
int32_t vm_now(void);

/* 実行結果 */
typedef enum {
    EXEC_DONE,    /* HALTに到達（ブロック完了） */
    EXEC_YIELD,   /* WAITでyield。exec_ms に待ち時間 */
    EXEC_BUDGET,  /* 命令数バジェット切れ。pcを保存して中断 */
    EXEC_ERROR    /* 実行時エラー（スタック溢れ等） */
} exec_status_t;

/* バイトコードを pc から実行する。
 *   pc      : 入出力。開始オフセット→（YIELD/BUDGET時）次回再開オフセット
 *   budget  : 実行可能な残り命令数（消費分を減算）
 *   out_ms  : EXEC_YIELD時のWAITミリ秒
 *   in_main : true ならWAIT(OP_YIELD)を許可（MAIN）。falseでYIELD遭遇は内部エラー
 * 文の境界をまたぐ協調実行はしない（1ブロック=1呼び出しで完走 or yield/budget中断）。
 */
exec_status_t vm_exec(uint16_t *pc, int *budget, int32_t *out_ms, bool in_main);

/* 文字列プールのオフセットからC文字列を得る（host出力ポートが使う） */
const char *vm_str(int32_t off);

/* ポート名→番号。見つからなければ -1（コンパイラ/テストが使う） */
int vm_find_port(const char *name);

/* STATUS 異常ビットを立てる（§12）。ISRからは呼ばない前提（tick文脈）。 */
void vm_set_err(int32_t bit);

/* 文字列値（SV_STR/SV_SREF）→ C文字列。それ以外は空文字（§4） */
const char *vm_resolve_str(value_t v);
/* 文字列スロットへコピー（切り詰め＋ERR_STR_TRUNC、自己コピー安全）。
 * kind=SSLOT_SVAR/SGVAR/SRESULT, idx は SRESULT時0。 */
void vm_store_sstr(int kind, int idx, const char *src);

#endif /* VM_H */
