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

/* 現在tick(ms)。スケジューラ/タイマが使う単調クロック。
 * 仕様の慣用名 "NOW"（def_in で束縛）を内部クロックとして用いる（§3）。 */
int32_t vm_now(void)
{
    script_vm_t *m = g_vm;
    int i;
    if (!m) return 0;
    for (i = 0; i < m->nports; i++) {
        if (m->ports[i].kind == PK_IN && m->ports[i].get_fn &&
            strcmp(m->ports[i].name, "NOW") == 0) {
            return m->ports[i].get_fn();
        }
    }
    return 0;
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

        if (*budget <= 0) { *pc = p; return EXEC_BUDGET; }
        (*budget)--;

        ip = p;
        op = code[p++];

        switch (op) {
        case OP_HALT:
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

        case OP_POP:
            m->sp--;
            break;

        default:
            *pc = ip;
            return EXEC_ERROR;
        }
    }
}
