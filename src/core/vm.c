/* vm.c - スタック型VMの実行エンジン（仕様 §1, §4, §5, §9, §15 フェーズ1）
 *
 * 全状態は単一の script_vm_t（アリーナ上）に置く。ここではバイトコード実行
 * （vm_exec）とアリーナ配置・補助（vm_now / vm_str）を担う。
 * スケジューラ本体（script_tick）とコンパイラは別ファイル。
 *
 * ホスト非依存（純C）コア層。
 */
#include <string.h>
#include "vm.h"
#include "script.h"   /* ERR_xxx ビット定義（§12） */

/* アリーナ先頭に置いた唯一のVMインスタンス */
static script_vm_t *g_vm = NULL;

script_vm_t *vm(void) { return g_vm; }

/* script_init から呼ぶ：アリーナにVMを配置しゼロ初期化する。
 * 戻り値 0=ok / <0=アリーナ不足 */
int vm_place_arena(void *arena, size_t size)
{
    if (!arena || size < sizeof(script_vm_t)) return -1;
    g_vm = (script_vm_t *)arena;
    memset(g_vm, 0, sizeof(*g_vm));
    g_vm->init_blk = -1;
    g_vm->main_blk = -1;
    return 0;
}

const char *vm_str(int32_t off)
{
    if (!g_vm || off < 0 || off >= g_vm->strpool_len) return "";
    return &g_vm->strpool[off];
}

/* now_fn を解決する（未設定ならポート表を1回だけ走査してキャッシュ）。見つからなければ NULL。
 * 旧来の script_register_in("NOW",…) だけで束ねたホストも、初回以降は O(1) になる（§8, v0.4.8）。 */
static in_fn_t resolve_now_fn(script_vm_t *m)
{
    int i;
    if (m->now_fn) return m->now_fn;
    for (i = 0; i < m->nports; i++) {
        if (m->ports[i].kind == PK_IN && m->ports[i].get_fn &&
            strcmp(m->ports[i].name, "NOW") == 0) {
            m->now_fn = m->ports[i].get_fn;   /* キャッシュ */
            return m->now_fn;
        }
    }
    return NULL;
}

/* 現在tick(ms)。スケジューラ/タイマが使う単調クロック（NOW）。now_fn を O(1) で呼ぶ。
 * 源が無ければ 0（この状態はロード時 ERR_NO_CLOCK で弾かれるので通常到達しない, §8, v0.4.8）。 */
int32_t vm_now(void)
{
    script_vm_t *m = g_vm;
    in_fn_t fn;
    if (!m) return 0;
    fn = resolve_now_fn(m);
    return fn ? fn() : 0;
}

/* クロック源が解決できるか（ロード時ガード用）。副作用: 見つかれば now_fn にキャッシュ。 */
int vm_has_clock(void)
{
    return (g_vm && resolve_now_fn(g_vm) != NULL) ? 1 : 0;
}

/* タイマスロットを1本確保し fire_time を設定（§8）。満杯なら無視＋フラグ。
 * INITフェーズ（!init_done）では「t0起点の相対ms」を仮置きし、INIT→RUN切替で t0 を足す
 * （v0.3.4：INITで張ったTIMERの締切は t0 起点で解決）。 */
static void vm_arm_timer(int32_t ms)
{
    script_vm_t *m = g_vm;
    int i;
    for (i = 0; i < CFG_TIMER_SLOTS; i++) {
        if (!m->timers[i].active) {
            m->timers[i].active = true;
            if (!m->init_done) {                 /* INITフェーズ：相対msを仮置き */
                m->timers[i].fire_time  = ms;
                m->timers[i].init_armed = true;
            } else {                             /* RUNフェーズ：arm時刻起点 */
                m->timers[i].fire_time  = vm_now() + ms;
                m->timers[i].init_armed = false;
            }
            return;
        }
    }
    m->timer_overflow = true;   /* 満杯：無視＋フラグ（§8） */
}

void vm_set_err(int32_t bit)    /* STATUS 異常ビット（§12） */
{
    if (g_vm) g_vm->status |= bit;
}

/* 文字列スロットのバッファ先頭（書き込み可）を返す。範囲外はNULL。 */
static char *sslot_buf(int kind, int idx)
{
    script_vm_t *m = g_vm;
    switch (kind) {
        case SSLOT_SVAR:    return (idx >= 0 && idx < CFG_SVAR_COUNT)  ? m->svar[idx]  : NULL;
        case SSLOT_SGVAR:   return (idx >= 0 && idx < CFG_SGVAR_COUNT) ? m->sgvar[idx] : NULL;
        case SSLOT_SRESULT: return m->sresult;
        case SSLOT_SARG:    return (idx >= 0 && idx < CFG_SARG_COUNT)  ? m->sarg[idx]  : NULL;  /* 受信専用・読みのみ */
    }
    return NULL;
}

const char *vm_resolve_str(value_t v)
{
    if (v.tag == SV_STR)  return vm_str(v.i);
    if (v.tag == SV_SREF) { char *b = sslot_buf(v.i >> 8, v.i & 0xFF); return b ? b : ""; }
    return "";
}

/* 文字列Utility共有の作業スクラッチ（arena内・容量 CFG_SSTR_LEN, §3）。
 * Utilityは排他・ワンパス（再入なし）なので1本を使い回してよい。入力引数はプール定数か
 * 文字列スロットで、このスクラッチをエイリアスすることは無い＝独立バッファとして安全。 */
char *vm_strtmp(void) { return g_vm ? g_vm->strtmp : NULL; }

void vm_store_sstr(int kind, int idx, const char *src)
{
    char *dst = sslot_buf(kind, idx);
    size_t n;
    if (!dst || !src) return;
    if (dst == src) return;                 /* 自己コピーは何もしない（strcpy UB回避, §4） */
    n = strlen(src);
    if (n >= CFG_SSTR_LEN) {                /* 切り詰め＋ERR_STR_TRUNC（§4,§12） */
        n = CFG_SSTR_LEN - 1;
        vm_set_err(ERR_STR_TRUNC);
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ---- バイトコード実行 ---- */
exec_status_t vm_exec(uint16_t *pc, int *budget, int32_t *out_ms, bool in_main)
{
    script_vm_t *m = g_vm;
    const uint8_t *code = m->code;
    uint16_t p = *pc;

    for (;;) {
        uint16_t ip;          /* この命令の開始位置（budget中断時の再開点） */
        uint8_t  op;

        if (*budget <= 0) {
            /* ループ内・スクリプト内ポート呼び出し中は命令の途中で切らない（式評価中に sp を失うと壊れる）。
             * REPEAT の後退辺 OP_REPEAT_NEXT が文境界で打ち切る（§7, v0.4.4）。ポート呼びは within-tick で
             * 完走させる（call_sp==0 まで戻ってから中断判定, §7 v0.4.5）。 */
            if (m->loop_sp == 0 && m->call_sp == 0) { *pc = p; return EXEC_BUDGET; }
        }
        (*budget)--;

        ip = p;
        op = code[p++];

        switch (op) {
        case OP_HALT:
            /* スクリプト内ポート本体からの HALT は呼び出し元へ return（§7, v0.4.5）。
             * call_sp>0 なら ARG/SARG を復帰し ret_pc へ戻る。call_sp==0 は従来のブロック完了。 */
            if (m->call_sp > 0) {
                call_frame_t *f = &m->callstack[--m->call_sp];
                memcpy(m->arg,  f->save_arg,  sizeof(m->arg));
                memcpy(m->sarg, f->save_sarg, sizeof(m->sarg));
                memcpy(m->var,  f->save_var,  sizeof(m->var));   /* VAR/SVAR も復帰（private, v0.4.6） */
                memcpy(m->svar, f->save_svar, sizeof(m->svar));
                p = f->ret_pc;
                break;
            }
            *pc = p;
            return EXEC_DONE;

        case OP_PUSH_INT: {
            int32_t v = (int32_t)((uint32_t)code[p] | ((uint32_t)code[p+1] << 8) |
                                  ((uint32_t)code[p+2] << 16) | ((uint32_t)code[p+3] << 24));
            p += 4;
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_int(v);
            break;
        }
        case OP_PUSH_STR: {
            uint16_t off = (uint16_t)(code[p] | (code[p+1] << 8));
            p += 2;
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_str((int32_t)off);
            break;
        }

        case OP_LOAD_GVAR: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = m->gvar[idx];
            break;
        }
        case OP_LOAD_VAR: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = m->var[idx];
            break;
        }
        case OP_LOAD_ARG: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = m->arg[idx];   /* 型タグ保持（char echo等, §10） */
            break;
        }
        case OP_LOAD_RESULT:
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = m->result;
            break;

        case OP_LOAD_PORT: {
            uint8_t pi = code[p++];
            int32_t v = m->ports[pi].get_fn ? m->ports[pi].get_fn() : 0;
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_int(v);
            break;
        }

        case OP_STORE_GVAR: {
            uint8_t idx  = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc;
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            /* 左→右評価済み。先頭(最左)を格納、残りは切り捨て、無ければ0埋め（§5） */
            m->gvar[idx] = (argc > 0) ? m->stack[base] : val_int(0);
            m->sp = base;
            break;
        }
        case OP_STORE_VAR: {
            uint8_t idx  = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc;
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            m->var[idx] = (argc > 0) ? m->stack[base] : val_int(0);
            m->sp = base;
            break;
        }

        case OP_LOAD_SVAR: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_sref(SSLOT_SVAR, idx);
            break;
        }
        case OP_LOAD_SGVAR: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_sref(SSLOT_SGVAR, idx);
            break;
        }
        case OP_LOAD_SRESULT:
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_sref(SSLOT_SRESULT, 0);
            break;
        case OP_LOAD_SARG: {
            uint8_t idx = code[p++];
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_sref(SSLOT_SARG, idx);   /* 受信文字列ビュー（§10） */
            break;
        }

        case OP_STORE_SSTR: {   /* 先頭1値をstrcpy（§4） */
            uint8_t kind = code[p++];
            uint8_t idx  = code[p++];
            value_t v;
            if (m->sp < 1) { *pc = ip; return EXEC_ERROR; }
            v = m->stack[--m->sp];
            vm_store_sstr(kind, idx, vm_resolve_str(v));
            break;
        }

        case OP_CALL_OUT: {
            uint8_t pi   = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc;
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            /* §4(v0.3.8): RESULT/SRESULT を更新するのは「値を産む的(産出型を持つ inout)」
             *     だけ＝set_fn が script_set_result/script_set_sresult を呼んだときのみ。
             *     産出none の out(STDOUT 等)送信は RESULT を一切触らない(VMは0クリアしない)。
             *     → `RESULT -> STDOUT` 等のデバッグ印字が RESULT を潰さない(旧仕様の罠を解消)。 */
            if (m->ports[pi].set_fn) m->ports[pi].set_fn(argc, &m->stack[base]);
            m->sp = base;       /* argc個を消費（過多はthunk側で無視＝切り捨て, §5） */
            break;
        }

        /* 比較（§9）: a=stack[sp-2], b=stack[sp-1] → bool */
        case OP_GT: case OP_LT: case OP_GE: case OP_LE: case OP_EQ: case OP_NE: {
            int32_t b = m->stack[--m->sp].i;
            int32_t a = m->stack[--m->sp].i;
            int r = 0;
            switch (op) {
                case OP_GT: r = (a >  b); break;
                case OP_LT: r = (a <  b); break;
                case OP_GE: r = (a >= b); break;
                case OP_LE: r = (a <= b); break;
                case OP_EQ: r = (a == b); break;
                case OP_NE: r = (a != b); break;
            }
            m->stack[m->sp++] = val_bool(r);
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: {
            int32_t b = m->stack[--m->sp].i;
            int32_t a = m->stack[--m->sp].i;
            int32_t r = 0;
            switch (op) {
                case OP_ADD: r = a + b; break;
                case OP_SUB: r = a - b; break;
                case OP_MUL: r = a * b; break;
                case OP_DIV:                         /* /0 は結果0で継続＋ERR_DIVZERO（§9） */
                    if (b == 0) { vm_set_err(ERR_DIVZERO); r = 0; }
                    else        r = a / b;
                    break;
                case OP_MOD:
                    if (b == 0) { vm_set_err(ERR_DIVZERO); r = 0; }
                    else        r = a % b;
                    break;
                case OP_BAND: r = a & b; break;
                case OP_BOR:  r = a | b; break;
                case OP_BXOR: r = a ^ b; break;
                /* シフト量は 0..31 に正規化（未定義動作回避） */
                case OP_SHL:  r = (int32_t)((uint32_t)a << ((uint32_t)b & 31)); break;
                case OP_SHR:  r = a >> ((uint32_t)b & 31); break;  /* 算術シフト */
            }
            m->stack[m->sp++] = val_int(r);
            break;
        }
        case OP_NOT: {
            int32_t a = m->stack[--m->sp].i;
            m->stack[m->sp++] = val_bool(a == 0);
            break;
        }
        case OP_BNOT: {
            int32_t a = m->stack[--m->sp].i;
            m->stack[m->sp++] = val_int(~a);
            break;
        }
        case OP_NEG: {
            int32_t a = m->stack[--m->sp].i;
            m->stack[m->sp++] = val_int(-a);
            break;
        }
        case OP_CLEAR_ERR: {   /* ERR_xxx -> CLEAR_ERR（§12） */
            int32_t bits = m->stack[--m->sp].i;
            m->status &= ~bits;
            if (bits & ERR_QUEUE_OVF)  m->evq_overflow = false;
            if (bits & ERR_TIMER_FULL) m->timer_overflow = false;
            break;
        }

        case OP_JZ: {
            uint16_t t = (uint16_t)(code[p] | (code[p+1] << 8)); p += 2;
            if (m->stack[--m->sp].i == 0) p = t;
            break;
        }
        case OP_JNZ: {
            uint16_t t = (uint16_t)(code[p] | (code[p+1] << 8)); p += 2;
            if (m->stack[--m->sp].i != 0) p = t;
            break;
        }
        case OP_JMP: {
            uint16_t t = (uint16_t)(code[p] | (code[p+1] << 8)); p += 2;
            p = t;
            break;
        }

        case OP_YIELD: {   /* WAIT（§7）。MAIN以外で出現したら内部エラー */
            int32_t ms = m->stack[--m->sp].i;
            if (!in_main) { *pc = ip; return EXEC_ERROR; }
            *out_ms = ms;
            *pc = p;        /* 次回はYIELDの次から再開 */
            return EXEC_YIELD;
        }
        case OP_ARM_TIMER: { /* TIMER（§8）。ハンドラ内からも可 */
            int32_t ms = m->stack[--m->sp].i;
            vm_arm_timer(ms);
            break;
        }
        case OP_POST_HANDLER: { /* 式リスト -> <ハンドラ源>（HANDLER含む名前付きハンドラ, v0.3.7+） */
            uint8_t pi   = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc;
            extern int sched_enqueue_handler_vals(int handler_port, const value_t *pos, int n);
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            sched_enqueue_handler_vals(pi, &m->stack[base], argc);  /* 即post＝次tickで ON <名前> */
            m->sp = base;
            break;
        }

        case OP_POST_HANDLER_AFTER: { /* 値リスト -> ハンドラ AFTER <ms>（遅延post, §10 v0.4.7） */
            uint8_t pi   = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc - 1;      /* スタックは [payload×argc, delay]（delay が top） */
            extern int sched_post_after(int handler_port, const value_t *pos, int n, int32_t ms);
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            sched_post_after(pi, &m->stack[base], argc, m->stack[m->sp - 1].i);
            m->sp = base;                     /* payload と delay を消費 */
            break;
        }

        case OP_POP:
            m->sp--;
            break;

        /* ---- 有界ループ（§7, v0.4.4） ---- */
        case OP_REPEAT_INIT: {   /* U16 end : N=pop。N<=0 は本体を飛ばす。else フレームを積む */
            uint16_t end = (uint16_t)(code[p] | (code[p+1] << 8)); p += 2;
            int32_t n = m->stack[--m->sp].i;
            if (n <= 0) { p = end; break; }                  /* 0回：本体スキップ */
            if (m->loop_sp >= CFG_LOOP_NEST) { *pc = ip; return EXEC_ERROR; }  /* 保険（compilerが上限を弾く） */
            m->loop[m->loop_sp].iter  = 1;
            m->loop[m->loop_sp].limit = n;
            m->loop_sp++;
            break;
        }
        case OP_REPEAT_NEXT: {   /* U16 top : 反復判定。バジェット切れは打ち切り＋ERR_BUDGET（within-tick） */
            uint16_t top = (uint16_t)(code[p] | (code[p+1] << 8)); p += 2;
            loop_frame_t *f;
            if (m->loop_sp <= 0) { *pc = ip; return EXEC_ERROR; }
            f = &m->loop[m->loop_sp - 1];
            if (*budget <= 0) {                              /* バジェット切れ：文境界で打ち切り（§7） */
                vm_set_err(ERR_BUDGET);
                m->loop_sp--;                                /* pop。p は NEXT の次＝ループ後を指す */
            } else if (++f->iter <= f->limit) {
                p = top;                                     /* 次の反復へ */
            } else {
                m->loop_sp--;                                /* 完了：pop して通過 */
            }
            break;
        }
        case OP_LOAD_ITR:        /* 最内ループの反復カウンタ（1..N）。REPEAT外なら0（compilerが弾く） */
            if (m->sp >= CFG_STACK_DEPTH) { *pc = ip; return EXEC_ERROR; }
            m->stack[m->sp++] = val_int(m->loop_sp > 0 ? m->loop[m->loop_sp - 1].iter : 0);
            break;

        /* ---- 動的添字アクセス（§4, v0.4.4）。argc==1=read / >=2=write。産出は RESULT/SRESULT ---- */
        case OP_INDEX: {
            uint8_t islot = code[p++];
            uint8_t argc  = code[p++];
            int base = m->sp - argc;
            int is_str = (islot >= ISLOT_SVAR);   /* SVAR/SGVAR/SARG が str */
            int32_t idx, i0;
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            idx = (argc >= 1) ? m->stack[base].i : 0;
            i0  = idx - 1;                        /* 1始まり → 0始まり */
            if (is_str) {
                int kind = (islot == ISLOT_SVAR)  ? SSLOT_SVAR  :
                           (islot == ISLOT_SGVAR) ? SSLOT_SGVAR : SSLOT_SARG;
                char *buf = sslot_buf(kind, i0);              /* 範囲外は NULL */
                if (argc >= 2) {                              /* write（ARG/SARG は compiler が弾く） */
                    const char *s = vm_resolve_str(m->stack[base + 1]);
                    if (buf) vm_store_sstr(kind, i0, s);      /* 範囲外は no-op */
                    script_set_sresult(buf ? s : "");         /* 産出＝書いた値（範囲外は空） */
                } else {                                      /* read */
                    script_set_sresult(buf ? buf : "");
                }
            } else {
                value_t *arr = (islot == ISLOT_VAR)  ? m->var  :
                               (islot == ISLOT_GVAR) ? m->gvar : m->arg;
                int count    = (islot == ISLOT_VAR)  ? CFG_VAR_COUNT  :
                               (islot == ISLOT_GVAR) ? CFG_GVAR_COUNT : CFG_ARG_COUNT;
                int in_range = (i0 >= 0 && i0 < count);
                if (argc >= 2) {                              /* write */
                    value_t v = m->stack[base + 1];
                    if (in_range) arr[i0] = v;
                    m->result = in_range ? v : val_int(0);
                } else {                                      /* read */
                    m->result = in_range ? arr[i0] : val_int(0);
                }
            }
            m->sp = base;
            break;
        }

        /* ---- スクリプト内ポート呼び出し（サブルーチン, §3 §7, v0.4.5） ---- */
        case OP_CALL_SCRIPT: {
            uint8_t pi   = code[p++];
            uint8_t argc = code[p++];
            int base = m->sp - argc;
            call_frame_t *f;
            int k;
            uint16_t body = m->ports[pi].bc_start;
            if (base < 0) { *pc = ip; return EXEC_ERROR; }
            if (m->call_sp >= CFG_CALL_NEST)  { *pc = ip; return EXEC_ERROR; } /* 静的に到達不能・防御 */
            if (body >= m->code_len)          { *pc = ip; return EXEC_ERROR; } /* 本体未定義/範囲外・防御 */
            /* caller の ARG/SARG/VAR/SVAR を退避（全て private ローカル・v0.4.6）。共有は GVAR/SGVAR のみ。 */
            f = &m->callstack[m->call_sp++];
            f->ret_pc = p;
            memcpy(f->save_arg,  m->arg,  sizeof(m->arg));
            memcpy(f->save_sarg, m->sarg, sizeof(m->sarg));
            memcpy(f->save_var,  m->var,  sizeof(m->var));
            memcpy(f->save_svar, m->svar, sizeof(m->svar));
            /* private 面をクリア（VAR/SVAR は真っさらなローカル、ARG/SARG は下で引数を充填） */
            for (k = 0; k < CFG_ARG_COUNT; k++)  m->arg[k] = val_int(0);
            for (k = 0; k < CFG_SARG_COUNT; k++) m->sarg[k][0] = '\0';
            for (k = 0; k < CFG_VAR_COUNT; k++)  m->var[k] = val_int(0);
            for (k = 0; k < CFG_SVAR_COUNT; k++) m->svar[k][0] = '\0';
            for (k = 0; k < argc && k < CFG_ARG_COUNT; k++) {
                value_t v = m->stack[base + k];
                if (val_is_str(v)) {                         /* str 位置 → SARG[k]（int ビューは 0 のまま） */
                    const char *s;
                    if (v.tag == SV_SREF && ((v.i >> 8) & 0xFF) == SSLOT_SARG) {
                        int si = v.i & 0xFF;                 /* SARG 参照は退避元から解決（充填で自壊するのを回避） */
                        s = (si >= 0 && si < CFG_SARG_COUNT) ? f->save_sarg[si] : "";
                    } else {
                        s = vm_resolve_str(v);               /* リテラル/SVAR/SGVAR/SRESULT は充填の影響を受けない */
                    }
                    if (k < CFG_SARG_COUNT) {
                        size_t n = strlen(s);
                        if (n >= CFG_SARG_LEN) { n = CFG_SARG_LEN - 1; vm_set_err(ERR_STR_TRUNC); }
                        memcpy(m->sarg[k], s, n);
                        m->sarg[k][n] = '\0';
                    } else {
                        vm_set_err(ERR_STR_TRUNC);           /* N_SARG 超の str 位置は落とす */
                    }
                } else {
                    m->arg[k] = v;                           /* int 位置 */
                }
            }
            m->sp = base;                                    /* 引数を消費 */
            p = body;                                        /* 本体へジャンプ */
            break;
        }
        case OP_STORE_RESULT:                                /* 値付き EXIT の int 戻り（pop → RESULT） */
            if (m->sp < 1) { *pc = ip; return EXEC_ERROR; }
            m->result = m->stack[--m->sp];
            break;

        default:
            *pc = ip;
            return EXEC_ERROR;
        }
    }
}
