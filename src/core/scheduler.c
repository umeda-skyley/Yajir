/* scheduler.c - 協調スケジューラ＋イベントキュー＋タイマ（仕様 §1, §8, §10, §15 フェーズ3,4）
 *
 * script_tick() の1ステップ:
 *   0. 初回のみ INIT を実行し、周期ONの初期発火時刻を決める
 *   1. タイマ満期 → ON TIMER をキューへ post（§8）
 *   2. イベントキューを drain → 該当 ON ハンドラへディスパッチ（§10）
 *   3. 周期 ON（時間源）を満期分実行（§6）
 *   4. MAIN を1スライス進める（WAITでyield/再開, §7）
 *
 * イベントキューは固定長リング。生産側(sched_post/enqueue)は生ISRから直接呼べる
 * （複数生産者＝ISR＋スクリプトのself-post）想定で「積んで即return」。溢れは新着
 * ドロップ＋オーバーフローフラグ（§10）。MPSC安全化のため enqueue のみクリティカル
 * セクション（YJ_ENTER/EXIT_CRITICAL・既定空／ホスト再定義, §11 v0.4.1）で保護する。
 *
 * ホスト非依存（純C）コア層。
 */
#include <string.h>
#include "scheduler.h"
#include "vm.h"

/* ---- イベントキュー（リングバッファ） ---- */

/* 生産側：1件積む。生ISRから直接呼べる（ISR＋self-postの複数生産者・単一消費者）。
 * スロット確保→ペイロード書込→tail公開 の全体をクリティカルセクションで囲い、
 * 生産者間の evq_tail 競合を防ぐ（消費側=sched_tick は保護不要・single-consumer）。
 * 既定の YJ_*_CRITICAL は空マクロ＝単一生産者ホストではコスト0（§11 v0.4.1）。0=ok / <0=満杯 */
static int evq_push(const event_t *ev)
{
    script_vm_t *m = vm();
    int rc;
    YJ_ENTER_CRITICAL();
    {
        int next = (m->evq_tail + 1) % CFG_EVENT_QUEUE_LEN;
        if (next == m->evq_head) {        /* 満杯：新着ドロップ＋フラグ（§10） */
            m->evq_overflow = true;
            rc = -1;
        } else {
            m->evq[m->evq_tail] = *ev;
            m->evq_tail = next;           /* 最後に公開 */
            rc = 0;
        }
    }
    YJ_EXIT_CRITICAL();
    return rc;
}

/* 消費はディスパッチ側（sched_tick の drain）でインラインに行う（境界スナップショット）。 */

/* ---- イベント組み立て（多値・型振り分け, v0.3.5） ---- */

/* 1つの str 位置を sstr[k] へコピー（k<N_SARG）。len<0 は strlen（終端あり）、len>=0 は明示長。
 * 超過/位置外は切詰＋ERR_STR_TRUNC。明示長によりC側の非終端バッファも安全に渡せる（§11, v0.3.8）。 */
static void put_str_pos(event_t *ev, int k, const char *s, int len)
{
    if (k < CFG_SARG_COUNT) {
        size_t L = (len < 0) ? strlen(s) : (size_t)len;
        if (L >= CFG_SARG_LEN) { L = CFG_SARG_LEN - 1; vm_set_err(ERR_STR_TRUNC); }
        memcpy(ev->sstr[k], s, L);
        ev->sstr[k][L] = '\0';
        ev->pos[k] = val_str(0);            /* STRマーカ（中身は sstr[k]） */
    } else {
        vm_set_err(ERR_STR_TRUNC);          /* N_SARG超の文字列位置は落とす（benign 0） */
        ev->pos[k] = val_int(0);
    }
}

/* 空イベントを初期化（全位置 benign 0 / 空文字） */
static void ev_clear(event_t *ev, event_kind_t kind, int handler_port, int npos)
{
    int k;
    ev->kind = kind; ev->handler_port = handler_port;
    ev->npos = (npos > CFG_ARG_COUNT) ? CFG_ARG_COUNT : npos;  /* 過多切り捨て（§5） */
    for (k = 0; k < CFG_ARG_COUNT; k++)  ev->pos[k] = val_int(0);
    for (k = 0; k < CFG_SARG_COUNT; k++) ev->sstr[k][0] = '\0';
}

/* VMスタック上の値（int/文字列参照）からイベントを組み立てる。文字列はここで sstr へコピーされる
 * ＝post時スナップショット（遅延post でも満期まで元 SVAR の変化に影響されない, §10 v0.4.7）。 */
static void build_event_vals(event_t *ev, event_kind_t kind, int handler_port, const value_t *pos, int n)
{
    int k;
    ev_clear(ev, kind, handler_port, n);
    for (k = 0; k < ev->npos; k++) {
        if (val_is_str(pos[k])) put_str_pos(ev, k, vm_resolve_str(pos[k]), -1);  /* 解決済み＝終端あり */
        else                    ev->pos[k] = pos[k];   /* INT */
    }
}

/* 組み立てて即キューへ（ハンドラ源/単値post共用） */
static int push_event_vals(event_kind_t kind, int handler_port, const value_t *pos, int n)
{
    event_t ev;
    build_event_vals(&ev, kind, handler_port, pos, n);
    return evq_push(&ev);
}

/* ---- 公開：非同期源 → VMへの橋（§10, §11） ---- */

/* INITフェーズ中はイベントの橋を閉じる（v0.3.4, §10）。割り込み起源含む全postを
 * no-op で破棄（フラグも立てない）。INIT→RUN切替でキューは空リセット済み。 */
static int bridge_closed(void) { return !vm()->init_done; }

int sched_post(const char *name, value_t v)   /* 単値（post_msg/char） */
{
    int pi;
    if (bridge_closed()) return 0;             /* INIT中：破棄（受理扱い・フラグ無し） */
    pi = vm_find_port(name);
    if (pi < 0 || vm()->ports[pi].kind != PK_HANDLER) return -1;
    return push_event_vals(EVT_HANDLER, pi, &v, 1);
}

int sched_post_v(const char *name, const script_arg_t *args, int n)   /* 多値（§11） */
{
    int pi; event_t ev; int k;
    if (bridge_closed()) return 0;             /* INIT中：破棄 */
    pi = vm_find_port(name);
    if (pi < 0 || vm()->ports[pi].kind != PK_HANDLER) return -1;
    ev_clear(&ev, EVT_HANDLER, pi, n);
    for (k = 0; k < ev.npos; k++) {
        switch (args[k].type) {
            case SCRIPT_ARG_T_INT:  ev.pos[k] = val_int(args[k].i);  break;   /* CHAR は撤去＝INT に統合(v0.4.2) */
            case SCRIPT_ARG_T_STR:  put_str_pos(&ev, k, args[k].s ? args[k].s : "", args[k].s ? args[k].len : 0); break;
        }
    }
    return evq_push(&ev);
}

int sched_enqueue_handler_vals(int handler_port, const value_t *pos, int n)   /* … -> HANDLER / MYHANDLER */
{
    if (bridge_closed()) return 0;             /* INIT中の名前付きpostは破棄（v0.3.4） */
    return push_event_vals(EVT_HANDLER, handler_port, pos, n);   /* ON <名前> へ（Cからのpostと同経路） */
}

/* 遅延post（`値リスト -> ハンドラ AFTER <ms>`, §10 v0.4.7）。pending 表に1枠取って payload を
 * コピーし、due を張る。満期は sched_tick が拾って通常キューへ流す（以後は普通のpostと同一経路）。
 *
 * INIT中でも破棄しない（TIMER の作法）＝スクリプト自身が書いた「予約」だから。橋を閉じる目的は
 * 外来イベントの t0 スタンピード防止であって、自分で張った予約を消すことではない。INIT中は due を
 * t0 起点の相対msとして持ち、transition_to_run で絶対時刻へ解決する（vm_arm_timer と同手口）。
 *
 * 戻り値: 0=ok / <0=pending満杯（新着ドロップ＋ERR_DELAY_FULL） */
int sched_post_after(int handler_port, const value_t *pos, int n, int32_t ms)
{
    script_vm_t *m = vm();
    int i;
    /* RUNフェーズの ms<=0 は即post に縮退（正常な退化・エラーにしない）。
     * INIT中は縮退させない——即postは橋で破棄されてしまうため、due=0 で張って t0 に発火させる。 */
    if (m->init_done && ms <= 0) return sched_enqueue_handler_vals(handler_port, pos, n);
    if (ms < 0) ms = 0;

    for (i = 0; i < CFG_DELAY_SLOTS; i++) {
        if (!m->delay[i].active) {
            build_event_vals(&m->delay[i].ev, EVT_HANDLER, handler_port, pos, n);  /* payloadをコピー */
            m->delay[i].active = true;
            if (!m->init_done) {                 /* INITフェーズ：t0起点の相対msを仮置き */
                m->delay[i].due        = ms;
                m->delay[i].init_armed = true;
            } else {                             /* RUNフェーズ：post時刻起点 */
                m->delay[i].due        = vm_now() + ms;
                m->delay[i].init_armed = false;
            }
            return 0;
        }
    }
    vm_set_err(ERR_DELAY_FULL);   /* 満杯：新着を捨てる（§10, §12） */
    return -1;
}

/* ---- ブロック検索 ---- */
static int find_block_kind(block_kind_t k)
{
    script_vm_t *m = vm();
    int i;
    for (i = 0; i < m->nblocks; i++) if (m->blocks[i].kind == k) return i;
    return -1;
}
static int find_handler_block(int handler_port)
{
    script_vm_t *m = vm();
    int i;
    for (i = 0; i < m->nblocks; i++)
        if (m->blocks[i].kind == BLK_ON_HANDLER && m->blocks[i].handler_port == handler_port) return i;
    return -1;
}

/* ---- ハンドラ実行（最後まで走り切る, §7） ----
 * argc = このブロックが受け取った位置の個数（入力ポート ARGC の値, v0.4.12）。
 * イベント経路は ev->npos、引数を持たない周期ON等は 0。**引数にしてあるのは呼び出し側で
 * 決め忘れを構造的に防ぐため**（fill_args はイベント経路でしか呼ばれない）。 */
static void run_handler(int bi, int argc)
{
    uint16_t pc = vm()->blocks[bi].bc_start;
    int budget = CFG_INSTR_BUDGET;     /* ハンドラ毎のバジェット（§1） */
    int32_t ms = 0;
    vm()->argc_cur = argc;
    vm()->sp = 0;
    vm()->loop_sp = 0;   /* ループフレームは block ごとにリセット（EXIT/エラーの途中脱出保険, v0.4.4） */
    vm()->call_sp = 0;   /* コールフレームも block ごとにリセット（within-tick 保険, v0.4.5） */
    /* budget切れは暴走ガード（REPEAT は後退辺で打ち切り＝ERR_BUDGET, §7 v0.4.4）。 */
    vm_exec(&pc, &budget, &ms, /*in_main=*/false);
}

/* 条件トリガの条件式を評価する（§6, v0.4.7）。条件チャンクは `<式> HALT` の独立コードなので、
 * ここから実行して HALT で止まり、スタック先頭に値が1つ残る。それを真偽として読むだけ
 * （＝専用opcodeを持たずに式を評価できる）。条件は純粋式で WAIT を含めないので yield しない。 */
static int eval_cond(const block_t *b)
{
    script_vm_t *m = vm();
    uint16_t pc = b->cond_start;
    int budget = CFG_INSTR_BUDGET;
    int32_t ms = 0;
    int r;
    m->argc_cur = 0;   /* 条件式も引数を持たない文脈（v0.4.12） */
    m->sp = 0; m->loop_sp = 0; m->call_sp = 0;
    vm_exec(&pc, &budget, &ms, /*in_main=*/false);
    r = (m->sp > 0) ? val_truthy(m->stack[0]) : 0;   /* 評価できなければ偽に縮退 */
    m->sp = 0;
    return r;
}

/* ARG[]/SARG[] をイベントで充填（型タグはpost側が決定, §10, v0.3.5）。
 * 位置 k は型で2ビューに振り分け、不一致/不足は benign（0/空文字）。 */
static void fill_args(const event_t *ev)
{
    script_vm_t *m = vm();
    int k;
    for (k = 0; k < CFG_ARG_COUNT; k++)
        m->arg[k] = (k < ev->npos && ev->pos[k].tag != SV_STR) ? ev->pos[k] : val_int(0);
    for (k = 0; k < CFG_SARG_COUNT; k++) {
        if (k < ev->npos && ev->pos[k].tag == SV_STR)
            memcpy(m->sarg[k], ev->sstr[k], CFG_SARG_LEN);   /* 終端付きでコピー済み */
        else
            m->sarg[k][0] = '\0';
    }
}

/* ---- MAIN 1スライス（WAITでyield/再開, §7） ---- */
static void advance_main(int32_t now)
{
    script_vm_t *m = vm();
    main_ctx_t *c = &m->main_ctx;
    int budget = CFG_INSTR_BUDGET;
    int32_t ms = 0;
    exec_status_t s;

    if (!c->started) return;
    if (c->waiting) {
        if (now < c->wake_time) return;   /* まだ待ち */
        c->waiting = false;               /* 起床。pcはWAITの次を指している */
    }
    m->argc_cur = 0;   /* MAIN は引数を受け取らない＝ARGC 0（v0.4.12） */
    m->sp = 0;   /* MAINは文境界(=WAIT)でyieldするのでスタックは空 */
    m->loop_sp = 0;   /* REPEAT は within-tick で完結（WAITまたぎ不可）ゆえ常に0（保険, v0.4.4） */
    m->call_sp = 0;   /* スクリプト内ポート呼びも within-tick で完結（保険, v0.4.5） */
    s = vm_exec(&c->pc, &budget, &ms, /*in_main=*/true);
    if (s == EXEC_YIELD) {
        c->waiting = true;
        c->wake_time = now + ms;
    } else if (s == EXEC_DONE) {
        c->pc = m->blocks[m->main_blk].bc_start;  /* 1パス完了→次tickで再周回 */
    } else if (s == EXEC_BUDGET) {
        /* pc保存済み。次tickで継続（暴走時もメインループは回る, §1） */
    } else { /* EXEC_ERROR */
        c->started = false;               /* MAINを停止（ホストは無傷, §12） */
    }
}

/* INIT→RUN 切替（t0 = now）。世界を始動する（v0.3.4, §1, §6, §8, §10）。 */
static void transition_to_run(int32_t now)
{
    script_vm_t *m = vm();
    int i;
    m->init_done = true;                       /* RUNフェーズへ */

    /* 周期ONの初回締切は t0 起点（長いINIT WAITでも catch-up債務を生まない, §6） */
    for (i = 0; i < m->nblocks; i++)
        if (m->blocks[i].kind == BLK_ON_PERIOD)
            m->blocks[i].next_time = now + m->blocks[i].period;

    /* INITで張ったTIMERの締切を t0 起点に解決（相対ms→絶対, §8） */
    for (i = 0; i < CFG_TIMER_SLOTS; i++)
        if (m->timers[i].active && m->timers[i].init_armed) {
            m->timers[i].fire_time  = now + m->timers[i].fire_time;
            m->timers[i].init_armed = false;
        }

    /* INITで張った遅延post も同じ作法で t0 起点に解決（§10, v0.4.7）。
     * これにより「INIT からスクリプト定義ハンドラの起動を予約する」が書ける。 */
    for (i = 0; i < CFG_DELAY_SLOTS; i++)
        if (m->delay[i].active && m->delay[i].init_armed) {
            m->delay[i].due        = now + m->delay[i].due;
            m->delay[i].init_armed = false;
        }

    /* INIT中は橋が閉じていたが、念のためキューを空リセット（§10） */
    m->evq_head = m->evq_tail = 0;

    /* 中断コンテキストを MAIN へ張り替え（INITと共用, §7） */
    if (m->main_blk >= 0) {
        m->main_ctx.started   = true;
        m->main_ctx.pc        = m->blocks[m->main_blk].bc_start;
        m->main_ctx.waiting   = false;
        m->main_ctx.wake_time = 0;
    } else {
        m->main_ctx.started = false;
    }
}

/* ---- INIT 1スライス（WAIT可・協調実行, v0.3.4）。完了で transition_to_run。 ---- */
static void advance_init(int32_t now)
{
    script_vm_t *m = vm();
    main_ctx_t *c = &m->main_ctx;   /* INITフェーズではINITを保持 */
    int budget = CFG_INSTR_BUDGET;
    int32_t ms = 0;
    exec_status_t s;

    if (m->init_blk < 0 || !c->started) { transition_to_run(now); return; }  /* INIT無し */
    if (c->waiting) {
        if (now < c->wake_time) return;   /* INIT WAIT中：世界は凍結のまま */
        c->waiting = false;
    }
    m->argc_cur = 0;  /* INIT は引数を受け取らない＝ARGC 0（v0.4.12） */
    m->sp = 0;
    m->loop_sp = 0;   /* v0.4.4 */
    m->call_sp = 0;   /* v0.4.5 */
    s = vm_exec(&c->pc, &budget, &ms, /*in_main(yield可)=*/true);  /* WAITをINITでも許可 */
    if (s == EXEC_YIELD) {
        c->waiting = true;
        c->wake_time = now + ms;          /* INIT中のWAIT。なお ARG[] は0（§6） */
    } else if (s == EXEC_DONE) {
        transition_to_run(now);           /* INIT脱出＝t0。RUN始動（同tickで以降を実行） */
    } else if (s == EXEC_BUDGET) {
        /* pc保存済み。次tickでINIT継続 */
    } else { /* EXEC_ERROR */
        transition_to_run(now);           /* 最善努力でRUNへ */
    }
}

/* ---- 1 tick ---- */
void sched_tick(void)
{
    script_vm_t *m = vm();
    int32_t now;
    int i;
    event_t ev;

    if (!m || !m->loaded) return;
    now = vm_now();

    /* 0) INITフェーズ（世界は凍結）：INITを1スライス進める。
     *    INITが完了した tick はそのまま RUN を同tickで動かす（t0=now）。
     *    INITが継続中（WAIT中など）はここで return＝周期/キュー/MAINは動かさない。 */
    if (!m->init_done) {
        advance_init(now);
        if (!m->init_done) return;        /* まだINIT中 → 世界凍結 */
    }

    /* 1) タイマ満期 → ON TIMER をキューへ（§8） */
    for (i = 0; i < CFG_TIMER_SLOTS; i++) {
        if (m->timers[i].active && now >= m->timers[i].fire_time) {
            event_t te;
            m->timers[i].active = false;     /* ワンショット（§8） */
            ev_clear(&te, EVT_TIMER, -1, 0); /* ON TIMER は ARG 無し（§8） */
            evq_push(&te);
        }
    }

    /* 1.5) 遅延post の満期 → キューへ（§10, v0.4.7）。TIMER と同じ位置で流すので、
     *      満期した遅延postは下の drain で同tick中にディスパッチされる（通常postと同一経路）。
     *      キューが満杯なら evq_push が ERR_QUEUE_OVF を立てる（pending満杯の ERR_DELAY_FULL と別）。 */
    for (i = 0; i < CFG_DELAY_SLOTS; i++) {
        if (m->delay[i].active && !m->delay[i].init_armed && now >= m->delay[i].due) {
            m->delay[i].active = false;          /* ワンショット（キャンセル機構は持たない） */
            evq_push(&m->delay[i].ev);
        }
    }

    /* 2) キューを drain → ディスパッチ（§10）。TIMER満期もこの経路（§8）。
     *
     * このtick開始時点でキューにある分だけを処理する（境界を先にスナップショット）。
     * ディスパッチ中に積まれたイベント（= … -> HANDLER の自己post）は境界より後ろなので
     * 「次tick以降」に回る（v0.3.6: self-post反復は1段ずつ＝1 tick 1ステップ）。
     * これにより無限 self-post でも1 tick内で固まらず、メインループは回り続ける（§1）。 */
    {
        int boundary = m->evq_tail;          /* tick開始時のキュー末尾を固定 */
        while (m->evq_head != boundary) {
            int bi = -1;
            ev = m->evq[m->evq_head];
            m->evq_head = (m->evq_head + 1) % CFG_EVENT_QUEUE_LEN;
            switch (ev.kind) {
                case EVT_HANDLER:     bi = find_handler_block(ev.handler_port); break;   /* HANDLER含む名前付きハンドラ */
                case EVT_TIMER:   bi = find_block_kind(BLK_ON_TIMER); break;
            }
            if (bi >= 0) { fill_args(&ev); run_handler(bi, ev.npos); }   /* ARGC=受け取った位置数 */
            /* 対応ブロックが無いイベントは捨てる */
        }
    }

    /* 3) 周期ON（時間源, §6）。取りこぼし方針は TS_PERIODIC_CATCHUP で切替（script_config.h）。
     *
     * どちらのモードも next_time は += period（周期グリッド start + k*period 上）で進め、
     * now へ再ベースしない。違いは「取りこぼしを順次消化するか／畳んでスキップするか」だけ。 */
    for (i = 0; i < m->nblocks; i++) {
        block_t *b = &m->blocks[i];
        if (b->kind != BLK_ON_PERIOD) continue;
        if (now < b->next_time) continue;

        if (b->cond_start != 0xFFFF) {
            /* 条件トリガ（エッジ, §6 v0.4.7）: 満期ごとに条件を評価し「前回偽・今回真」でだけ発火。
             * 偽になれば prev が落ちて自動リセット＝再成立で再発火する。
             * 注: 条件付きでは catch-up（取りこぼし消化）は無意味（本質は回数でなくエッジ）。
             *
             * prev には「本体が走った後」の条件値を入れる。本体が自分のトリガ条件を落とす場合
             * （`ON 10 (STATUS & ERR_x)` … `ERR_x -> CLEAR_ERR` のようなフラグ消費型）に、
             * clear をそのまま再武装として扱うため。走る前の値を入れると、clear 直後・次の満期前に
             * 条件が再成立したとき prev が true のままとなり、以後永久に発火しなくなる。
             * 再評価は発火したときだけ＝エッジは稀なのでコストは無視できる。 */
            int cond = eval_cond(b);
            if (cond && !b->prev) {
                run_handler(i, 0);          /* 周期ONは引数を受け取らない＝ARGC 0 */
                cond = eval_cond(b);   /* 本体が条件を落としたかを見る（落ちていれば即再武装） */
            }
            b->prev = (bool)cond;
        } else {
            run_handler(i, 0);                      /* 1 tick につき1回は必ず発火（ARGC 0） */
        }
        if (b->period <= 0) {
            b->next_time = now + 1;                 /* 0周期の退避（無限ループ防止） */
            continue;
        }
        b->next_time += b->period;                  /* 次の満期へ（1周期だけ進める） */
#if !TS_PERIODIC_CATCHUP
        /* SMOOTH: 取りこぼした満期は畳んでグリッド上の次の点までスキップ＝位相を即復帰。
         * CATCHUP（既定）ではスキップせず、残りの満期は次tick以降で1回ずつ順次消化する
         * （バーストを出さず、過負荷時は実時刻に遅れて追従＝ドリフトを無理に復帰しない）。 */
        while (b->next_time <= now) b->next_time += b->period;
#endif
    }

    /* 4) MAIN 1スライス（§7） */
    advance_main(now);
}
