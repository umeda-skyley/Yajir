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
    PK_HANDLER,  /* def_handler : ON <NAME> ハンドラ源         */
    PK_SCRIPT    /* def_port    : スクリプト内ポート（本体=PORTブロック・両辺可・産出型あり, v0.4.5） */
} port_kind_t;

typedef struct {
    char          name[CFG_MAX_NAME];
    port_kind_t   kind;
    script_type_t out_type;   /* 産出型（PK_IN / PK_INOUT / PK_SCRIPT で有効, v0.3.8/v0.4.5）。out/handler/const は未使用 */
    int32_t       const_val;  /* PK_CONST */
    in_fn_t       get_fn;     /* PK_IN / PK_INOUT（関数ポートは NULL） */
    out_fn_t      set_fn;     /* PK_OUT / PK_INOUT */
    uint16_t      bc_start;   /* PK_SCRIPT: 本体バイトコード開始オフセット（0xFFFF=本体未定義, v0.4.5） */
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
    /* ---- 条件トリガ `ON <周期> (条件)`（エッジ, §6 v0.4.7） ----
     * cond_start: 条件式チャンク（`<式> HALT`・本体より前に置かれる）の開始。0xFFFF=条件なし。
     * prev: 前回評価の真偽。満期ごとに評価し「prev偽・今回真」でだけ本体を実行、偽で自動リセット。
     *       ロード時 false ゆえ、t0 後の初回満期で条件が真なら立ち上がりとみなして発火する。 */
    uint16_t     cond_start;
    bool         prev;
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

/* ---- ループフレーム（§7 REPEAT, v0.4.4）。WAIT 禁止ゆえティックをまたがない（within-tick）。 ---- */
typedef struct {
    int32_t iter;    /* 現在の反復カウンタ（1..limit）＝ ITR */
    int32_t limit;   /* 入口でスナップショットした N */
} loop_frame_t;

/* ---- コールフレーム（§3 §7 スクリプト内ポート, v0.4.5/v0.4.6）。WAIT 禁止ゆえティックをまたがない。
 * ARG/SARG/VAR/SVAR は全てポートの private ローカル＝呼び出しで退避＋クリアし、戻り時に caller の値を
 * 復元する（v0.4.6：VAR/SVAR も private 化＝ポートが caller の作業変数を壊さない「真の関数」に）。
 * 共有され続けるのは GVAR/SGVAR（大域）だけ。戻り値は RESULT/SRESULT。
 * RAM: 1フレーム ≈ ARG+SARG+VAR+SVAR の退避。SVAR(CFG_SVAR_COUNT×CFG_SSTR_LEN)が支配項ゆえ、
 * CFG_CALL_NEST を実際の最大呼び出し深度（ロード時DFSで検算済）まで絞るとRAMを直接削れる。 */
typedef struct {
    uint16_t ret_pc;                            /* 呼び出し元の再開オフセット */
    int      save_argc;                         /* caller の ARGC（受け取った位置数, v0.4.12） */
    value_t  save_arg[CFG_ARG_COUNT];           /* caller ARG[] の退避（受信引数） */
    char     save_sarg[CFG_SARG_COUNT][CFG_SARG_LEN]; /* caller SARG[] の退避 */
    value_t  save_var[CFG_VAR_COUNT];           /* caller VAR[] の退避（v0.4.6 private ローカル化） */
    char     save_svar[CFG_SVAR_COUNT][CFG_SSTR_LEN];  /* caller SVAR[] の退避（同上） */
} call_frame_t;

/* 動的添字アクセス（N -> SLOT, §4 v0.4.4）のスロット識別（OP_INDEX オペランド）。
 * VAR/GVAR/ARG=int スロット、SVAR/SGVAR/SARG=str スロット。ARG/SARG は read のみ（受信専用）。 */
enum { ISLOT_VAR = 0, ISLOT_GVAR = 1, ISLOT_ARG = 2, ISLOT_SVAR = 3, ISLOT_SGVAR = 4, ISLOT_SARG = 5 };

/* ---- タイマスロット（§8） ---- */
typedef struct {
    bool    active;
    int32_t fire_time;   /* この時刻(tick)以降で満期。init_armed時は t0 起点の相対ms（オフセット） */
    bool    init_armed;  /* INITフェーズで張った→締切は t0 起点で解決（v0.3.4, §8） */
} timer_slot_t;

/* ---- 遅延post の pending スロット（§10, v0.4.7）。`値リスト -> ハンドラ AFTER <ms>`。
 * payload は post 時にコピーして保持（満期まで元 SVAR が変わっても届く値は post 時のもの）。
 * 満期時に通常のイベントキューへ enqueue＝以後は普通の post と完全に同一経路。
 * INITフェーズで張った分は TIMER と同じ作法で t0 起点解決（破棄しない・§10）。 */
typedef struct {
    bool    active;
    bool    init_armed;  /* INITで張った→due は t0 起点の相対ms（transition_to_run で絶対化） */
    int32_t due;         /* この時刻(tick)以降で満期 */
    event_t ev;          /* 送るイベント（handler_port と payload を保持）＝RAMの支配項 */
} delay_slot_t;

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
    /* 実行中のブロック/ポートが受け取った「位置の個数」＝入力ポート ARGC の実体（§10, v0.4.12）。
     * これで受け側が `none`（0個）と `0`（1個・値0）を区別できる（ARG[k] は不足を benign 0 で
     * 返すため両者が同じに見えていた）。引数を受け取らないブロック（INIT/MAIN/周期ON/ON TIMER）は 0。
     * PORT 呼び出しでは call_frame_t に退避・復帰する（ARG/SARG と同じ private 扱い）。 */
    int     argc_cur;
    value_t result;

    /* 文字列スロット（§4 文字列スロット・案A）。固定長バッファ。 */
    char    sgvar[CFG_SGVAR_COUNT][CFG_SSTR_LEN];
    char    svar[CFG_SVAR_COUNT][CFG_SSTR_LEN];
    char    sresult[CFG_SSTR_LEN];
    /* 文字列Utility共有の作業スクラッチ（§3）。Utilityは排他・ワンパス（再入なし）ゆえ1本で足りる。
     * ここに置くことで関数スタックが CFG_SSTR_LEN に比例して膨らむのを防ぐ（大きくなるのは arena 側）。 */
    char    strtmp[CFG_SSTR_LEN];

    /* 異常フラグ集合（§12 STATUS）。ERR_xxx ビットの論理和。 */
    int32_t status;

    /* xorshift32 PRNG 状態（RAND/SEED, §3 v0.4.3）。0は縮退ゆえ mathutil 側で既定シードへ落とす。 */
    uint32_t rng_state;

    /* ループフレームスタック（§7 REPEAT, v0.4.4）。REPEAT は within-tick ゆえ block 実行内で
     * push/pop が完結し、tick をまたがない（loop_sp は各 block 開始時 0）。 */
    loop_frame_t loop[CFG_LOOP_NEST];
    int          loop_sp;

    /* コールフレームスタック（§3 §7 スクリプト内ポート, v0.4.5）。深さは load 時の静的DAG判定で
     * CFG_CALL_NEST 以内を検算済み（実行時溢れは到達不能・防御assertのみ）。各 block 開始時 0。 */
    call_frame_t callstack[CFG_CALL_NEST];
    int          call_sp;

    /* ポート表（§3） */
    port_t  ports[CFG_MAX_PORTS];
    int     nports;

    /* コア昇格した2つのホスト注入点（§11, v0.4.8）。ポート表を名前で引かずここを直参照する。
     *   now_fn      : 内部クロック（NOW）。vm_now() が O(1) で呼ぶ。未設定なら初回走査でキャッシュ。
     *   stdout_puts : STDOUT のシンク。出力ポリシー本体はコアの th_stdout_core が持つ。 */
    in_fn_t  now_fn;
    void   (*stdout_puts)(const char *s);
    /* def_import のライブラリ取得（§13, v0.4.10）。コンパイル時にだけ呼ばれる。NULL=import非対応。 */
    script_import_fn import_fn;

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

    /* 遅延post の pending 表（§10, v0.4.7）。tick文脈しか触らない（ISRからは触らない）ので
     * クリティカルセクション不要。満期時に保護済みの evq_push 経路でキューへ流す。 */
    delay_slot_t delay[CFG_DELAY_SLOTS];
} script_vm_t;

/* 単一インスタンスへのアクセス（アリーナ上に配置） */
script_vm_t *vm(void);

/* script_init から：アリーナにVMを配置しゼロ初期化。0=ok / <0=不足 */
int     vm_place_arena(void *arena, size_t size);
/* スケジューラ/タイマ用の単調クロック。now_fn を O(1) で呼ぶ（未設定なら初回だけポート表を
 * 走査して "NOW" を now_fn にキャッシュ）。源が無ければ 0（§3, §8, v0.4.8）。 */
int32_t vm_now(void);
/* クロック源が解決できるか（now_fn or ポート表の "NOW"）。ロード時ガードで使う。副作用として
 * 見つかれば now_fn にキャッシュする（§8, v0.4.8）。 */
int     vm_has_clock(void);

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

/* 文字列Utility用の共有作業スクラッチ（arena内・容量 CFG_SSTR_LEN）。§3。 */
char *vm_strtmp(void);

#endif /* VM_H */
