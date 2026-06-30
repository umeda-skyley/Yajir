/* scheduler.c - 協調スケジューラ＋イベントキュー＋タイマ（仕様 §1, §8, §10, §15 フェーズ3,4）
 *
 * script_tick() の1ステップ:
 *   0. 初回のみ INIT を実行し、周期ONの初期発火時刻を決める
 *   1. タイマ満期 → ON TIMER をキューへ post（§8）
 *   2. イベントキューを drain → 該当 ON ハンドラへディスパッチ（§10）
 *   3. 周期 ON（時間源）を満期分実行（§6）
 *   4. MAIN を1スライス進める（WAITでyield/再開, §7）
 *
 * イベントキューは固定長SPSCリング。生産側(sched_post/enqueue)はISRから呼べる
 * 想定で「積んで即return」。溢れは新着ドロップ＋オーバーフローフラグ（§10）。
 *
 * ホスト非依存（純C）コア層。
 */
#include <string.h>
#include "scheduler.h"
#include "vm.h"

/* ---- イベントキュー（リングバッファ） ---- */

/* 生産側：1件積む。ISR安全に保つべきはこの操作のみ（§10）。
 * 実機ではここを短いクリティカルセクションで囲む。0=ok / <0=満杯 */
static int evq_push(const event_t *ev)
{
    script_vm_t *m = vm();
    int next = (m->evq_tail + 1) % CFG_EVENT_QUEUE_LEN;
    if (next == m->evq_head) {        /* 満杯：新着ドロップ＋フラグ（§10） */
        m->evq_overflow = true;
        return -1;
    }
    m->evq[m->evq_tail] = *ev;
    m->evq_tail = next;               /* 最後に公開（SPSC） */
    return 0;
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

/* VMスタック上の値（int/char/文字列参照）から組み立てる（ハンドラ源/単値post共用） */
static int push_event_vals(event_kind_t kind, int handler_port, const value_t *pos, int n)
{
    event_t ev; int k;
    ev_clear(&ev, kind, handler_port, n);
    for (k = 0; k < ev.npos; k++) {
        if (val_is_str(pos[k])) put_str_pos(&ev, k, vm_resolve_str(pos[k]), -1);  /* 解決済み＝終端あり */
        else                    ev.pos[k] = pos[k];   /* INT/CHAR */
    }
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
            case SCRIPT_ARG_T_INT:  ev.pos[k] = val_int(args[k].i);  break;
            case SCRIPT_ARG_T_CHAR: ev.pos[k] = val_char(args[k].i); break;
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

/* ---- ハンドラ実行（最後まで走り切る, §7） ---- */
static void run_handler(int bi)
{
    uint16_t pc = vm()->blocks[bi].bc_start;
    int budget = CFG_INSTR_BUDGET;     /* ハンドラ毎のバジェット（§1） */
    int32_t ms = 0;
    vm()->sp = 0;
    /* ループ構文が無い v0 ではハンドラは有界。budget切れは暴走ガード。 */
    vm_exec(&pc, &budget, &ms, /*in_main=*/false);
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
    m->sp = 0;   /* MAINは文境界(=WAIT)でyieldするのでスタックは空 */
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
    m->sp = 0;
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
            if (bi >= 0) { fill_args(&ev); run_handler(bi); }
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

        run_handler(i);                             /* 1 tick につき1回は必ず発火 */
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
