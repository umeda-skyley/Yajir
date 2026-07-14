/* compiler.c - ワンパス・コンパイラ（仕様 §2, §5, §7, §13, §15 フェーズ2）
 *
 * トークン列を直接バイトコードへ。ジャンプはバックパッチ。
 *   - 複数引数のargc付与、過不足の正規化（§5）
 *   - WAIT位置チェック（MAIN専用, §7）
 *   - 添字範囲・未定義ポート・ネスト上限などを行番号付きで報告（§11, §12）
 * エラーは setjmp/longjmp で巻き戻す。
 *
 * 本文中の def_* 行はC側マクロの写し（§13）として読み飛ばす。
 * ホスト非依存（純C）コア層。
 */
#include <string.h>
#include <stdio.h>
#include <setjmp.h>
#include "compiler.h"
#include "tokenizer.h"
#include "vm.h"

#define MAX_CHAIN 32   /* 1つの AND/OR 連鎖あたりの最大項数 */

typedef struct {
    lexer_t  lx;
    token_t  cur;
    int      yield_ok;    /* WAIT許可フラグ（MAIN/INITコンパイル中か, §7, v0.3.4） */
    int      nest;        /* IFYESネスト深さ（§7） */
    int      repeat_depth;/* REPEATループのネスト深さ（WAIT禁止・ITR有効判定, §7 v0.4.4） */
    int      cur_port_type;/* コンパイル中のスクリプト内ポートの産出型 TY_INT/TY_STR、-1=非ポート（§7 v0.4.5・EXIT型検査） */
    int      cur_port_sidx;/* 同・script-port index（DAG辺記録用）、-1=非ポート（v0.4.5） */
    int      open_line;   /* 直近に開いたブロック/IFYESのヘッダ行（END欠落の aux 用, v0.3.7） */
    jmp_buf  jb;
} parser_t;

static parser_t P;
static script_error_t s_error;   /* 直近のロード失敗（構造化, v0.3.7 §11） */

/* ---- エイリアス（def_alias v0.3.8 ／ def_local v0.4.6）。純コンパイル時の名前解決＝VM は一切関与しない ----
 * def_alias（大域・ファイル冒頭）: GVAR/SGVAR（読み書き可）・数値・文字列（読取専用）。
 * def_local（局所・ブロック冒頭・END まで）: VAR/SVAR（読み書き可）・ARG/SARG（読取専用）・数値・文字列。
 *   局所分はブロック入口で s_nalias を記録し END でそこまで切り詰める（high-water-mark, §3 v0.4.6）。
 *   別ブロック間の同名は可（切り詰めで消える）、大域名との衝突/同一ブロック二重宣言は alias_find で弾く。 */
typedef enum { AL_GVAR, AL_SGVAR, AL_INT, AL_STR, AL_VAR, AL_SVAR, AL_ARG, AL_SARG } alias_kind_t;
typedef struct {
    char         name[CFG_MAX_NAME];
    alias_kind_t kind;
    int32_t      v;   /* スロット系=添字 / INT=値 / STR=文字列プールoffset */
} alias_t;
static alias_t s_aliases[CFG_MAX_ALIAS];
static int     s_nalias;

/* ---- スクリプト内ポート（def_port）の呼び出しグラフ（§3, v0.4.5） ----
 * コンパイル時のみの揮発状態。sidx（0..s_nscript_ports-1）を圧縮索引に使い、辺はビットセット
 * （callee sidx のビット）で持つ。ロード末尾に DFS でサイクル検出＋最長路（最大コールスタック段数）。 */
static int      s_script_ports[CFG_MAX_SCRIPT_PORTS];  /* sidx -> ポート表インデックス */
static uint32_t s_call_edges  [CFG_MAX_SCRIPT_PORTS];  /* sidx -> callee sidx のビットセット */
static int      s_nscript_ports;
static int      s_dfs_color   [CFG_MAX_SCRIPT_PORTS];  /* 0=white,1=gray,2=black */
static int      s_dfs_memo    [CFG_MAX_SCRIPT_PORTS];  /* 最長路メモ */
static int      s_cycle_pi;                            /* サイクル上のポート表インデックス（tok用） */

static int script_sidx_of(int pi)   /* ポート表index -> sidx（無ければ -1） */
{
    int i;
    for (i = 0; i < s_nscript_ports; i++) if (s_script_ports[i] == pi) return i;
    return -1;
}

/* PORT 本体をコンパイル中（P.cur_port_sidx>=0）に別のスクリプト内ポートを呼んだら、呼び出しグラフに
 * 辺を1本足す（DAG判定用, §3 v0.4.5）。本体外（INIT/MAIN/ON）からの呼びは根＝辺にしない。 */
static void record_call_edge(int callee_pi)
{
    int cs;
    if (P.cur_port_sidx < 0) return;
    cs = script_sidx_of(callee_pi);
    if (cs >= 0) s_call_edges[P.cur_port_sidx] |= (1u << cs);
}

static const alias_t *alias_find(const char *name)
{
    int i;
    for (i = 0; i < s_nalias; i++)
        if (strcmp(s_aliases[i].name, name) == 0) return &s_aliases[i];
    return NULL;
}
/* エイリアス名に使えない予約語（スロット名・制御語）。ポート/定数は vm_find_port で別途弾く。 */
static int is_reserved_name(const char *n)
{
    static const char *const kw[] = {
        "RESULT","SRESULT","GVAR","VAR","ARG","SGVAR","SVAR","SARG",
        "none","IFYES","WAIT","TIMER","CLEAR_ERR","NOT","AND","OR",
        "INIT","MAIN","ON","END","ELSE","EXIT","REPEAT","ITR","PORT", 0
    };
    int i;
    for (i = 0; kw[i]; i++) if (strcmp(n, kw[i]) == 0) return 1;
    return 0;
}

/* ---- エラー（構造化・文字列化はコアでしない / (B)方針） ---- */
static void fail_e(int line, script_err_t code, int aux, const char *tok)
{
    s_error.line = (int16_t)line;
    s_error.code = code;
    s_error.aux  = (int16_t)aux;
    if (tok) { strncpy(s_error.tok, tok, CFG_MAX_NAME - 1); s_error.tok[CFG_MAX_NAME - 1] = '\0'; }
    else     s_error.tok[0] = '\0';
    longjmp(P.jb, 1);
}
static void fail(int line, script_err_t code)                      { fail_e(line, code, 0, NULL); }
static void fail_tok(int line, script_err_t code, const char *tok) { fail_e(line, code, 0, tok); }
static void fail_end(int line, int aux)                            { fail_e(line, ERR_END_EXPECTED, aux, NULL); }

/* ---- トークン送り ---- */
static void adv(void)
{
    char eb[80]; int el = 0;
    if (lex_next(&P.lx, &P.cur, eb, sizeof(eb), &el) < 0)
        fail(el, ERR_SYNTAX);   /* 字句エラーは構文崩れに集約（メッセージは捨てる） */
}
static int is_kw(const char *kw)
{
    return P.cur.type == T_IDENT && strcmp(P.cur.text, kw) == 0;
}
static void expect(tok_type t, const char *what)
{
    (void)what;   /* 文字列化はコアでしない。種別は ERR_SYNTAX に集約 */
    if (P.cur.type != t) fail(P.cur.line, ERR_SYNTAX);
    adv();
}
/* 文末：改行 or EOF を消費（最終行の改行省略を許す） */
static void expect_newline(void)
{
    if (P.cur.type == T_NEWLINE) { adv(); return; }
    if (P.cur.type == T_EOF) return;
    fail(P.cur.line, ERR_SYNTAX);
}
/* 文末。鎖の中間に置けない終端（スロット書き等）の直後に '->' があれば位置違反（§5, v0.4） */
static void end_stmt(int line)
{
    if (P.cur.type == T_ARROW) fail(line, ERR_BAD_POSITION);   /* この終端は鎖の中間になれない */
    expect_newline();
}
static void skip_newlines(void)
{
    while (P.cur.type == T_NEWLINE) adv();
}

/* ---- バイトコード出力 ---- */
static void need(int n)
{
    if (vm()->code_len + n > CFG_CODE_SIZE)
        fail(P.cur.line, ERR_SYNTAX);   /* バイトコード溢れ（資源限界・受け皿） */
}
static void emit8(int b)     { need(1); vm()->code[vm()->code_len++] = (uint8_t)b; }
static void emit_i32(int32_t v){ need(4); emit8(v); emit8(v>>8); emit8(v>>16); emit8(v>>24); }
static int  here(void)       { return vm()->code_len; }
/* ジャンプ命令を出し、U16オペランド位置（パッチ先）を返す */
static int  emit_jump(int op){ emit8(op); need(2); { int s = here(); emit8(0); emit8(0); return s; } }
static void patch(int site, int target)
{
    vm()->code[site]   = (uint8_t)target;
    vm()->code[site+1] = (uint8_t)(target >> 8);
}

/* ---- ブロック表 ---- */
static int add_block(block_kind_t kind)
{
    script_vm_t *m = vm();
    block_t *b;
    if (m->nblocks >= CFG_MAX_BLOCKS) fail(P.cur.line, ERR_SYNTAX);   /* ブロック数上限（資源） */
    b = &m->blocks[m->nblocks];
    memset(b, 0, sizeof(*b));
    b->kind = kind;
    b->bc_start = (uint16_t)here();
    b->handler_port = -1;
    return m->nblocks++;
}

/* ---- 式 ---- */
static void parse_expr(void);

static int read_index(int limit, const char *slot)
{
    int idx;
    expect(T_LBRACKET, "'['");
    if (P.cur.type != T_NUMBER) fail_tok(P.cur.line, ERR_BAD_SLOT_INDEX, slot);  /* 添字が定数でない */
    idx = P.cur.num;
    adv();
    expect(T_RBRACKET, "']'");
    if (idx < 0 || idx >= limit) fail_tok(P.cur.line, ERR_BAD_SLOT_INDEX, slot); /* 範囲外（tok=スロット名） */
    return idx;
}

/* 直近に解析した(部分)式の静的型（型分離のため, §4） */
#define TY_INT 0
#define TY_STR 1
static int g_type;        /* parse_primary 等が設定 */
static int g_first_type;  /* 直近の expr_list の先頭要素の型 */
static int g_last_type;   /* 直近の expr_list の末尾要素の型（動的添字ライトの値型チェック用, v0.4.4） */
static int g_str_reads;   /* この expr_list 中の str産出ポート読みの数（単一SRESULT衝突回避, v0.3.8） */

static void need_int(int line)
{
    if (g_type != TY_INT)
        fail(line, ERR_TYPE_MISMATCH);   /* 式中で文字列（算術/比較は int, 文字列は EQUALS） */
}

/* 単独 EXIT / PORT本体フォールスルーの「戻り型のデフォルト値を産出して HALT」（§7, v0.4.5）。
 * str は SRESULT="" （int 0 を OP_STORE_SSTR に渡すと vm_resolve_str が "" を返す）、int/handler は RESULT=0。 */
static void emit_exit_default(int type)
{
    emit8(OP_PUSH_INT); emit_i32(0);
    if (type == TY_STR) { emit8(OP_STORE_SSTR); emit8(SSLOT_SRESULT); emit8(0); }
    else                { emit8(OP_STORE_RESULT); }
    emit8(OP_HALT);
}

static void parse_primary(void)
{
    int line = P.cur.line;
    g_type = TY_INT;                 /* 既定はint。文字列のときだけ下で TY_STR にする */
    switch (P.cur.type) {
    case T_NUMBER:
        emit8(OP_PUSH_INT); emit_i32(P.cur.num); adv(); return;
    case T_STRING:
        emit8(OP_PUSH_STR); { int o = P.cur.str_off; emit8(o); emit8(o >> 8); } adv();
        g_type = TY_STR; return;
    case T_LPAREN:
        adv(); parse_expr(); expect(T_RPAREN, "')'"); return;  /* g_type は内側が設定 */
    case T_IDENT: break;
    default:
        fail(line, ERR_SYNTAX);   /* 式が来るべき所に式が無い */
    }

    /* 識別子 */
    {
        char name[CFG_MAX_NAME];
        int pi;
        strcpy(name, P.cur.text);

        if (!strcmp(name, "RESULT")) { adv(); emit8(OP_LOAD_RESULT); return; }
        if (!strcmp(name, "ITR")) {   /* 反復カウンタ（REPEAT 内だけ有効・int, §7 v0.4.4） */
            if (P.repeat_depth <= 0) fail(line, ERR_SYNTAX);
            adv(); emit8(OP_LOAD_ITR); return;
        }
        if (!strcmp(name, "GVAR")) { adv(); { int i = read_index(CFG_GVAR_COUNT, "GVAR"); emit8(OP_LOAD_GVAR); emit8(i); } return; }
        if (!strcmp(name, "VAR"))  { adv(); { int i = read_index(CFG_VAR_COUNT,  "VAR");  emit8(OP_LOAD_VAR);  emit8(i); } return; }
        if (!strcmp(name, "ARG"))  { adv(); { int i = read_index(CFG_ARG_COUNT,  "ARG");  emit8(OP_LOAD_ARG);  emit8(i); } return; }
        /* 文字列スロット読み（§4）。型は TY_STR。 */
        if (!strcmp(name, "SRESULT")) { adv(); emit8(OP_LOAD_SRESULT); g_type = TY_STR; return; }
        if (!strcmp(name, "SGVAR")) { adv(); { int i = read_index(CFG_SGVAR_COUNT, "SGVAR"); emit8(OP_LOAD_SGVAR); emit8(i); } g_type = TY_STR; return; }
        if (!strcmp(name, "SVAR"))  { adv(); { int i = read_index(CFG_SVAR_COUNT,  "SVAR");  emit8(OP_LOAD_SVAR);  emit8(i); } g_type = TY_STR; return; }
        if (!strcmp(name, "SARG"))  { adv(); { int i = read_index(CFG_SARG_COUNT,  "SARG");  emit8(OP_LOAD_SARG);  emit8(i); } g_type = TY_STR; return; }  /* 受信専用・読みのみ（§10） */
        if (!strcmp(name, "none"))
            fail(line, ERR_SYNTAX);   /* 'none' は引数リスト全体のときだけ */

        /* エイリアス（def_alias）：スロット/数値/文字列に展開（コンパイル時, v0.3.8） */
        {
            const alias_t *al = alias_find(name);
            if (al) {
                adv();
                switch (al->kind) {
                case AL_GVAR:  emit8(OP_LOAD_GVAR);  emit8((int)al->v); return;                       /* int */
                case AL_SGVAR: emit8(OP_LOAD_SGVAR); emit8((int)al->v); g_type = TY_STR; return;
                case AL_INT:   emit8(OP_PUSH_INT);   emit_i32(al->v); return;                          /* int */
                case AL_STR:   emit8(OP_PUSH_STR);   emit8((int)al->v); emit8((int)(al->v >> 8)); g_type = TY_STR; return;
                case AL_VAR:   emit8(OP_LOAD_VAR);   emit8((int)al->v); return;                        /* 局所int, v0.4.6 */
                case AL_SVAR:  emit8(OP_LOAD_SVAR);  emit8((int)al->v); g_type = TY_STR; return;
                case AL_ARG:   emit8(OP_LOAD_ARG);   emit8((int)al->v); return;                        /* 受信int */
                case AL_SARG:  emit8(OP_LOAD_SARG);  emit8((int)al->v); g_type = TY_STR; return;       /* 受信str */
                }
            }
        }

        pi = vm_find_port(name);
        if (pi < 0) fail_tok(line, ERR_UNKNOWN_NAME, name);   /* 未登録の名前（tok=名前） */
        adv();
        switch (vm()->ports[pi].kind) {
        case PK_CONST: emit8(OP_PUSH_INT); emit_i32(vm()->ports[pi].const_val); return;
        case PK_IN:
        case PK_INOUT:
            emit8(OP_LOAD_PORT); emit8(pi);   /* get_fn を呼ぶ。str産出なら get_fn が SRESULT を書く */
            /* 産出型を式の静的型へ反映（v0.3.8）。int産出はそのまま戻り値が積まれる。
             * str産出（READER 等）は「戻り int を捨て、産んだ文字列＝SRESULT への参照を積む」。
             * 新opcode不要＝LOAD_PORT(副作用でSRESULT更新) + POP + LOAD_SRESULT（M3）。 */
            if (vm()->ports[pi].out_type == SCRIPT_T_STR) {
                emit8(OP_POP); emit8(OP_LOAD_SRESULT);
                g_type = TY_STR;
                g_str_reads++;   /* 単一SRESULT: 1リストに str源読みは1個まで（parse_expr_list で検査） */
            }
            return;
        case PK_SCRIPT:   /* スクリプト内ポートを値源として読む＝引数0で同期呼び（ARG全0, §3 v0.4.5） */
            emit8(OP_CALL_SCRIPT); emit8(pi); emit8(0);
            record_call_edge(pi);
            if (vm()->ports[pi].out_type == SCRIPT_T_STR) {
                emit8(OP_LOAD_SRESULT); g_type = TY_STR; g_str_reads++;   /* 単一SRESULT: 1リスト1個まで */
            } else {
                emit8(OP_LOAD_RESULT);  g_type = TY_INT;
            }
            return;
        case PK_OUT:     fail_tok(line, ERR_BAD_POSITION, name);   /* out は左辺に立てられない（産出none・値源でない） */
        case PK_HANDLER: fail_tok(line, ERR_BAD_POSITION, name);   /* ハンドラ源は左辺に立てられない（post先専用） */
        }
    }
}

/* 単項（§9）: NOT（論理否定） / ~（ビット反転） / -（符号反転）。連鎖可。被演算子はint */
static void parse_unary(void)
{
    int line = P.cur.line;
    if (is_kw("NOT"))           { adv(); parse_unary(); need_int(line); emit8(OP_NOT);  g_type = TY_INT; }
    else if (P.cur.type == T_TILDE) { adv(); parse_unary(); need_int(line); emit8(OP_BNOT); g_type = TY_INT; }
    else if (P.cur.type == T_MINUS) { adv(); parse_unary(); need_int(line); emit8(OP_NEG);  g_type = TY_INT; }
    else parse_primary();
}

/* 乗除剰余（§9）: * / %（両辺int） */
static void parse_mul(void)
{
    int line;
    parse_unary();
    for (;;) {
        line = P.cur.line;
        if      (P.cur.type == T_STAR)  { need_int(line); adv(); parse_unary(); need_int(line); emit8(OP_MUL); g_type = TY_INT; }
        else if (P.cur.type == T_SLASH) { need_int(line); adv(); parse_unary(); need_int(line); emit8(OP_DIV); g_type = TY_INT; }
        else if (P.cur.type == T_PCT)   { need_int(line); adv(); parse_unary(); need_int(line); emit8(OP_MOD); g_type = TY_INT; }
        else break;
    }
}

static void parse_add(void)
{
    parse_mul();
    while (P.cur.type == T_PLUS || P.cur.type == T_MINUS) {
        int line = P.cur.line; tok_type t = P.cur.type;
        need_int(line); adv(); parse_mul(); need_int(line);
        emit8(t == T_PLUS ? OP_ADD : OP_SUB); g_type = TY_INT;
    }
}

/* シフト（§9）: << >>（両辺int） */
static void parse_shift(void)
{
    parse_add();
    while (P.cur.type == T_SHL || P.cur.type == T_SHR) {
        int line = P.cur.line; tok_type t = P.cur.type;
        need_int(line); adv(); parse_add(); need_int(line);
        emit8(t == T_SHL ? OP_SHL : OP_SHR); g_type = TY_INT;
    }
}

static void parse_cmp(void)
{
    int line, op = -1;
    parse_shift();
    switch (P.cur.type) {
    case T_GT: op = OP_GT; break; case T_LT: op = OP_LT; break;
    case T_GE: op = OP_GE; break; case T_LE: op = OP_LE; break;
    case T_EQ: op = OP_EQ; break; case T_NE: op = OP_NE; break;
    default: return;
    }
    line = P.cur.line;
    need_int(line); adv(); parse_shift(); need_int(line);
    emit8(op); g_type = TY_INT;
}

/* ビットAND（§9）: &（両辺int） */
static void parse_band(void)
{
    parse_cmp();
    while (P.cur.type == T_AMP) { int line = P.cur.line; need_int(line); adv(); parse_cmp(); need_int(line); emit8(OP_BAND); g_type = TY_INT; }
}
/* ビットXOR（§9）: ^ */
static void parse_bxor(void)
{
    parse_band();
    while (P.cur.type == T_CARET) { int line = P.cur.line; need_int(line); adv(); parse_band(); need_int(line); emit8(OP_BXOR); g_type = TY_INT; }
}
/* ビットOR（§9）: | */
static void parse_bor(void)
{
    parse_bxor();
    while (P.cur.type == T_PIPE) { int line = P.cur.line; need_int(line); adv(); parse_bxor(); need_int(line); emit8(OP_BOR); g_type = TY_INT; }
}

/* short-circuit AND（§9）: 全項真で1、いずれか偽で0 */
static void parse_and(void)
{
    int sites[MAX_CHAIN], n = 0, endsite, lfalse, i, line;
    parse_bor();
    if (!is_kw("AND")) return;
    need_int(P.cur.line);
    sites[n++] = emit_jump(OP_JZ);
    while (is_kw("AND")) {
        line = P.cur.line; adv(); parse_bor(); need_int(line);
        if (n >= MAX_CHAIN) fail(P.cur.line, ERR_SYNTAX);   /* AND連鎖が長すぎ（限界） */
        sites[n++] = emit_jump(OP_JZ);
    }
    emit8(OP_PUSH_INT); emit_i32(1);
    endsite = emit_jump(OP_JMP);
    lfalse = here();
    for (i = 0; i < n; i++) patch(sites[i], lfalse);
    emit8(OP_PUSH_INT); emit_i32(0);
    patch(endsite, here());
    g_type = TY_INT;
}

/* short-circuit OR（§9）: いずれか真で1、全項偽で0 */
static void parse_or(void)
{
    int sites[MAX_CHAIN], n = 0, endsite, ltrue, i, line;
    parse_and();
    if (!is_kw("OR")) return;
    need_int(P.cur.line);
    sites[n++] = emit_jump(OP_JNZ);
    while (is_kw("OR")) {
        line = P.cur.line; adv(); parse_and(); need_int(line);
        if (n >= MAX_CHAIN) fail(P.cur.line, ERR_SYNTAX);   /* OR連鎖が長すぎ（限界） */
        sites[n++] = emit_jump(OP_JNZ);
    }
    emit8(OP_PUSH_INT); emit_i32(0);
    endsite = emit_jump(OP_JMP);
    ltrue = here();
    for (i = 0; i < n; i++) patch(sites[i], ltrue);
    emit8(OP_PUSH_INT); emit_i32(1);
    patch(endsite, here());
    g_type = TY_INT;
}

static void parse_expr(void) { parse_or(); }

/* expr_list = expr {"," expr} | "none"。pushした数（argc）を返す（§5）。
 * 先頭要素の型を g_first_type に残す（スロット格納の型分離チェック用, §4）。 */
static int parse_expr_list(void)
{
    int n, line = P.cur.line;
    g_str_reads = 0;   /* このリストでの str産出ポート読みを数える（単一SRESULT衝突回避） */
    if (is_kw("none")) { adv(); g_first_type = g_last_type = TY_INT; return 0; }
    parse_expr(); n = 1; g_first_type = g_type;
    while (P.cur.type == T_COMMA) { adv(); parse_expr(); n++; }
    g_last_type = g_type;   /* 末尾要素の型（動的添字ライトの値型, v0.4.4） */
    /* str産出ポートの“読み”は1リストに1個まで。産んだ文字列は共有 SRESULT 1本に乗るため、
     * 2個読むと後の読みが前を上書きする（§4）。複数欲しいときは一旦 SVAR 等へ退避（純パイプは到達しない）。
     * 自動退避は将来ノブ。リテラル/スロット/SARG は各自実体を持つので制限なし。 */
    if (g_str_reads >= 2) fail(line, ERR_BAD_POSITION);
    return n;
}

/* arity 1 のポート向けに「最左の値だけ残す」よう正規化（§5） */
static void normalize_to_one(int argc)
{
    int i;
    if (argc == 0) { emit8(OP_PUSH_INT); emit_i32(0); }
    else for (i = 0; i < argc - 1; i++) emit8(OP_POP);  /* 上(後着)から捨て、最左を残す */
}

/* ---- 文 ---- */
static void parse_stmt_list(void);
static void parse_local_decls(void);   /* ブロック冒頭の def_local 列（定義は後方, v0.4.6） */

/* (式) -> IFYES … [ELSE …] END。条件は既にpush済み（argc個） */
static void parse_if(int argc, int line)
{
    int elsesite, endsite;
    int save_open = P.open_line;
    P.open_line = line;              /* この IFYES の開きヘッダ行（END欠落の aux 用） */
    P.nest++;
    if (P.nest > CFG_NEST_LIMIT) fail(line, ERR_NEST_TOO_DEEP);
    normalize_to_one(argc);
    expect_newline();
    elsesite = emit_jump(OP_JZ);     /* 偽ならELSE/ENDへ */
    parse_stmt_list();               /* THEN本体（ELSE/ENDで停止） */
    if (is_kw("ELSE")) {
        adv();
        endsite = emit_jump(OP_JMP);
        patch(elsesite, here());
        expect_newline();
        parse_stmt_list();           /* ELSE本体 */
        if (!is_kw("END")) fail_end(P.cur.line, line);
        adv();
        patch(endsite, here());
    } else {
        if (!is_kw("END")) fail_end(P.cur.line, line);
        adv();
        patch(elsesite, here());
    }
    P.nest--;
    P.open_line = save_open;
    expect_newline();
}

/* 裸のスロット名（添字なし）＝動的添字アクセスのターゲット判定（§4, v0.4.4）。該当で1を返し
 * *islot=ISLOT_*、*etype=TY_INT/TY_STR、*writable(ARG/SARGは0=受信専用) を設定。 */
static int slot_target(const char *name, int *islot, int *etype, int *writable)
{
    if (!strcmp(name, "VAR"))   { *islot = ISLOT_VAR;   *etype = TY_INT; *writable = 1; return 1; }
    if (!strcmp(name, "GVAR"))  { *islot = ISLOT_GVAR;  *etype = TY_INT; *writable = 1; return 1; }
    if (!strcmp(name, "ARG"))   { *islot = ISLOT_ARG;   *etype = TY_INT; *writable = 0; return 1; }
    if (!strcmp(name, "SVAR"))  { *islot = ISLOT_SVAR;  *etype = TY_STR; *writable = 1; return 1; }
    if (!strcmp(name, "SGVAR")) { *islot = ISLOT_SGVAR; *etype = TY_STR; *writable = 1; return 1; }
    if (!strcmp(name, "SARG"))  { *islot = ISLOT_SARG;  *etype = TY_STR; *writable = 0; return 1; }
    return 0;
}

/* 動的添字アクセスの1段を吐く（§4, v0.4.4）。argc>=2=write / ==1=read。第1引数=添字(int必須)。
 * write の値(g_last_type)はスロット要素型と一致必須。ARG/SARG への write は ERR_BAD_POSITION。
 * is_chain なら産出値(RESULT/SRESULT)を次段の暗黙左辺へ push する。 */
static void emit_index_stage(int islot, int etype, int writable, int argc, int is_chain, int line, const char *name)
{
    if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);            /* 添字は int */
    if (argc >= 2) {                                                      /* write */
        if (!writable)            fail_tok(line, ERR_BAD_POSITION, name); /* ARG/SARG は受信専用 */
        if (g_last_type != etype) fail(line, ERR_TYPE_MISMATCH);          /* 書く値の型 */
    }
    emit8(OP_INDEX); emit8(islot); emit8(argc);
    if (is_chain) {
        if (etype == TY_STR) { emit8(OP_LOAD_SRESULT); g_first_type = TY_STR; }
        else                 { emit8(OP_LOAD_RESULT);  g_first_type = TY_INT; }
    }
}

/* <N> -> REPEAT … END（有界ループ, §7 v0.4.4）。N は push 済み(argc個)＝正規化して1値に。
 * 本体内で WAIT は禁止（ERR_WAIT_IN_LOOP＝within-tick）。ネストは CFG_LOOP_NEST 段まで。
 * バイトコード:  <N> REPEAT_INIT L_END / L_TOP: <body> REPEAT_NEXT L_TOP / L_END:  */
static void parse_repeat(int argc, int line)
{
    int endsite, top;
    int save_open = P.open_line;
    P.open_line = line;
    if (P.repeat_depth >= CFG_LOOP_NEST) fail(line, ERR_NEST_TOO_DEEP);   /* ループネスト上限 */
    P.repeat_depth++;
    normalize_to_one(argc);                 /* N を1値に */
    expect_newline();
    endsite = emit_jump(OP_REPEAT_INIT);     /* pop N; N<=0 → endsite。else フレーム(iter=1,limit=N) */
    top = here();
    parse_stmt_list();                       /* 本体（END で停止） */
    if (!is_kw("END")) fail_end(P.cur.line, line);
    adv();
    emit8(OP_REPEAT_NEXT); emit8(top & 0xFF); emit8((top >> 8) & 0xFF);   /* 後退辺（top へ） */
    patch(endsite, here());                  /* N<=0 / バジェット打ち切りの落ち先＝ループ後 */
    P.repeat_depth--;
    P.open_line = save_open;
    expect_newline();
}

static void parse_stmt(void)
{
    int line = P.cur.line;
    char tname[CFG_MAX_NAME];
    int argc, pi;

    /* def_local はブロック冒頭一括のみ（文の後は不可, §3 v0.4.6）。ここに来た＝文の位置なので構文エラー。 */
    if (is_kw("def_local")) fail(line, ERR_SYNTAX);

    /* 単独 EXIT: 「戻り型のデフォルト値を返して抜ける」略記（§7 v0.4.5・後方互換）。
     * ハンドラ/T_INTポートは 0->EXIT、T_STRポートは ""->EXIT と同義＝END到達フォールスルーと同一。
     * IFYES の内側からでもブロック全体を抜ける（C の return 相当）。INIT/MAIN では構文エラー
     * （yield_ok が立つ＝MAIN/INIT。WAIT の "ON/PORT専用" 判定のちょうど逆）。
     * 値付き `値 -> EXIT` は下の終端段（tname=="EXIT"）で扱う。 */
    if (is_kw("EXIT")) {
        if (P.yield_ok) fail(line, ERR_SYNTAX);
        adv();
        emit_exit_default(P.cur_port_type < 0 ? TY_INT : P.cur_port_type);
        expect_newline();
        return;
    }

    argc = parse_expr_list();
    expect(T_ARROW, "'->'");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);   /* -> の後に送り先が無い */
    strcpy(tname, P.cur.text);
    adv();

    /* パイプライン・チェイン（§5, v0.4）：直後が '->' のセグメントは中間段＝inout のみ。
     * 「前段を呼ぶ → その産出値(int=RESULT/str=SRESULT)を次段の暗黙左辺へ1個LOAD」を繰り返し、
     * '->' で続かないセグメント（＝終端）で下の既存処理に合流する。純コンパイル時糖衣＝新opなし。 */
    while (P.cur.type == T_ARROW) {
        int islot, etype, writ;
        if (slot_target(tname, &islot, &etype, &writ)) {   /* 裸のスロット＝動的添字（中間段, §4 v0.4.4） */
            emit_index_stage(islot, etype, writ, argc, /*is_chain=*/1, line, tname);
            argc = 1;                                       /* 継ぎ目は単一値（産出をLOAD済み） */
            adv();                                          /* '->' を消費 */
            if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
            strcpy(tname, P.cur.text); adv();
            continue;
        }
        pi = vm_find_port(tname);
        if (pi < 0) fail_tok(line, ERR_UNKNOWN_PORT, tname);
        if (vm()->ports[pi].kind == PK_SCRIPT) {        /* スクリプト内ポート＝中間段OK（inout同格, §3 v0.4.5） */
            emit8(OP_CALL_SCRIPT); emit8(pi); emit8(argc);
            record_call_edge(pi);
        } else if (vm()->ports[pi].kind == PK_INOUT) {
            emit8(OP_CALL_OUT); emit8(pi); emit8(argc); /* 前段を呼ぶ（先頭はargc, 継ぎ目以降は1） */
        } else {
            fail_tok(line, ERR_BAD_POSITION, tname);    /* out/handler/in/const を中間に置けない */
        }
        if (vm()->ports[pi].out_type == SCRIPT_T_STR) { emit8(OP_LOAD_SRESULT); g_first_type = TY_STR; }
        else                                          { emit8(OP_LOAD_RESULT);  g_first_type = TY_INT; }
        argc = 1;                                       /* 継ぎ目は常に単一値（純パイプ・§5） */
        adv();                                          /* '->' を消費 */
        if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
        strcpy(tname, P.cur.text);
        adv();
    }

    /* ---- 終端段（tname）---- */

    /* 動的添字アクセス（裸のスロット名・添字なし）: N -> SLOT / idx,val -> SLOT（§4, v0.4.4）。
     * 括弧付き SLOT[k]（次トークンが '['）は下の定数添字処理へ回す。 */
    {
        int islot, etype, writ;
        if (slot_target(tname, &islot, &etype, &writ) && P.cur.type != T_LBRACKET) {
            emit_index_stage(islot, etype, writ, argc, /*is_chain=*/0, line, tname);
            end_stmt(line);
            return;
        }
    }

    /* 制御ポート（被送信値は int） */
    if (!strcmp(tname, "IFYES")) {
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);   /* 条件は int（文字列はEQUALS） */
        parse_if(argc, line); return;
    }
    if (!strcmp(tname, "REPEAT")) {                                  /* <N> -> REPEAT … END（有界ループ, §7 v0.4.4） */
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);   /* N は int */
        parse_repeat(argc, line); return;
    }
    if (!strcmp(tname, "WAIT")) {
        if (!P.yield_ok) fail(line, ERR_WAIT_IN_ON);                 /* ON内のWAIT（§7） */
        if (P.repeat_depth > 0) fail(line, ERR_WAIT_IN_LOOP);        /* REPEAT内のWAIT（§7 v0.4.4・within-tick） */
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_YIELD); expect_newline(); return;
    }
    if (!strcmp(tname, "TIMER")) {
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_ARM_TIMER); expect_newline(); return;
    }
    if (!strcmp(tname, "EXIT")) {   /* 値付きリターン `値 -> EXIT`（§7 v0.4.5） */
        if (P.yield_ok) fail(line, ERR_SYNTAX);            /* INIT/MAIN 不可 */
        normalize_to_one(argc);                            /* 最左1値を残す */
        if (P.cur_port_type < 0) {                         /* ハンドラ: 値を捨てて早期リターン */
            emit8(OP_POP); emit8(OP_HALT);
        } else if (P.cur_port_type == TY_STR) {            /* str ポート: 値(str)を SRESULT へ */
            if (g_first_type != TY_STR) fail(line, ERR_TYPE_MISMATCH);
            emit8(OP_STORE_SSTR); emit8(SSLOT_SRESULT); emit8(0); emit8(OP_HALT);
        } else {                                           /* int ポート: 値(int)を RESULT へ */
            if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
            emit8(OP_STORE_RESULT); emit8(OP_HALT);
        }
        expect_newline(); return;
    }
    /* 注: HANDLER はもう特殊分岐ではない。組込みの予約ハンドラ源チャネル（script_init で
     *     register 済み）として、下のポート検索→PK_HANDLER→OP_POST_HANDLER 経路を通る。 */
    if (!strcmp(tname, "CLEAR_ERR")) {   /* ERR_xxx -> CLEAR_ERR（§12） */
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_CLEAR_ERR); expect_newline(); return;
    }

    /* intスロット格納（型分離：文字列は不可, §4） */
    if (!strcmp(tname, "GVAR")) {
        int i = read_index(CFG_GVAR_COUNT, "GVAR");
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);   /* str→int スロット */
        emit8(OP_STORE_GVAR); emit8(i); emit8(argc); end_stmt(line); return;
    }
    if (!strcmp(tname, "VAR"))  {
        int i = read_index(CFG_VAR_COUNT, "VAR");
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        emit8(OP_STORE_VAR); emit8(i); emit8(argc); end_stmt(line); return;
    }
    /* read-only スロットへの送信＝役割違反（ERR_SYNTAX） */
    if (!strcmp(tname, "ARG"))    fail(line, ERR_SYNTAX);
    if (!strcmp(tname, "SARG"))   fail(line, ERR_SYNTAX);
    if (!strcmp(tname, "RESULT")) fail(line, ERR_SYNTAX);

    /* 文字列スロット格納（型分離：int は不可, §4）。strcpy 相当・切詰チェック */
    if (!strcmp(tname, "SGVAR")) {
        int i = read_index(CFG_SGVAR_COUNT, "SGVAR");
        if (g_first_type != TY_STR) fail(line, ERR_TYPE_MISMATCH);   /* int→str スロット */
        normalize_to_one(argc); emit8(OP_STORE_SSTR); emit8(SSLOT_SGVAR); emit8(i); end_stmt(line); return;
    }
    if (!strcmp(tname, "SVAR")) {
        int i = read_index(CFG_SVAR_COUNT, "SVAR");
        if (g_first_type != TY_STR) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_STORE_SSTR); emit8(SSLOT_SVAR); emit8(i); end_stmt(line); return;
    }
    if (!strcmp(tname, "SRESULT")) fail(line, ERR_SYNTAX);   /* SRESULTは読み取り専用（役割違反） */

    /* エイリアス（def_alias）への送信：GVAR/SGVAR は書込可、数値/文字列は読取専用→不可（v0.3.8） */
    {
        const alias_t *al = alias_find(tname);
        if (al) {
            switch (al->kind) {
            case AL_GVAR:
                if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
                emit8(OP_STORE_GVAR); emit8((int)al->v); emit8(argc); end_stmt(line); return;
            case AL_SGVAR:
                if (g_first_type != TY_STR) fail(line, ERR_TYPE_MISMATCH);
                normalize_to_one(argc); emit8(OP_STORE_SSTR); emit8(SSLOT_SGVAR); emit8((int)al->v); end_stmt(line); return;
            case AL_VAR:   /* 局所エイリアス VAR（読み書き可, v0.4.6） */
                if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
                emit8(OP_STORE_VAR); emit8((int)al->v); emit8(argc); end_stmt(line); return;
            case AL_SVAR:
                if (g_first_type != TY_STR) fail(line, ERR_TYPE_MISMATCH);
                normalize_to_one(argc); emit8(OP_STORE_SSTR); emit8(SSLOT_SVAR); emit8((int)al->v); end_stmt(line); return;
            case AL_ARG:
            case AL_SARG:
                fail_tok(line, ERR_BAD_POSITION, tname);   /* ARG/SARG 別名は受信専用（右辺不可） */
            case AL_INT:
            case AL_STR:
                fail_tok(line, ERR_BAD_POSITION, tname);   /* 数値/文字列エイリアスは読取専用（右辺不可） */
            }
        }
    }

    /* 出力ポート */
    pi = vm_find_port(tname);
    if (pi < 0) fail_tok(line, ERR_UNKNOWN_PORT, tname);     /* 未登録の送り先（tok=名前） */
    switch (vm()->ports[pi].kind) {
    case PK_OUT:
    case PK_INOUT: emit8(OP_CALL_OUT); emit8(pi); emit8(argc); break;
    case PK_SCRIPT: emit8(OP_CALL_SCRIPT); emit8(pi); emit8(argc); record_call_edge(pi); break;  /* サブルーチン呼び・戻り値は捨てる（§3 v0.4.5） */
    case PK_HANDLER:   emit8(OP_POST_HANDLER); emit8(pi); emit8(argc); break;  /* 名前付きHANDLER（スクリプトから自己/相互post, v0.3.7+） */
    case PK_IN:    fail_tok(line, ERR_BAD_POSITION, tname);   /* in は右辺に立てられない（値源・受信不可, §3） */
    case PK_CONST: fail_tok(line, ERR_BAD_POSITION, tname);   /* const は右辺に立てられない（値源） */
    }
    expect_newline();
}

static void parse_stmt_list(void)
{
    for (;;) {
        skip_newlines();
        if (is_kw("END") || is_kw("ELSE")) return;
        if (P.cur.type == T_EOF) fail_end(P.cur.line, P.open_line);   /* END前にEOF（開きヘッダ=aux） */
        parse_stmt();
    }
}

/* INIT / MAIN */
static void parse_simple_block(block_kind_t kind)
{
    int bi, amark;
    expect_newline();           /* ヘッダ行終端 */
    bi = add_block(kind);
    P.yield_ok = (kind == BLK_MAIN || kind == BLK_INIT);  /* WAITはMAIN/INITで可（v0.3.4） */
    P.nest = 0;
    P.repeat_depth = 0;
    P.cur_port_type = -1; P.cur_port_sidx = -1;   /* スクリプト内ポート本体ではない（v0.4.5） */
    amark = s_nalias;           /* ローカル別名スコープ入口（v0.4.6） */
    parse_local_decls();        /* 冒頭一括の def_local */
    parse_stmt_list();
    if (!is_kw("END")) fail_end(P.cur.line, P.open_line);  /* aux=このブロックの開きヘッダ行 */
    adv();
    emit8(OP_HALT);
    s_nalias = amark;           /* ブロック局所別名を捨てる（END まで, v0.4.6） */
    if (kind == BLK_INIT) vm()->init_blk = bi;
    else                  vm()->main_blk = bi;
    expect_newline();
}

/* ON <trigger> */
static void parse_on_block(void)
{
    block_kind_t kind;
    int period = 0, handler = -1;
    int line = P.cur.line;

    if (P.cur.type == T_NUMBER) { kind = BLK_ON_PERIOD; period = P.cur.num; adv(); }
    else if (P.cur.type == T_IDENT) {
        if (is_kw("TIMER"))   { kind = BLK_ON_TIMER;   adv(); }
        /* ON HANDLER も特別扱いをやめ、下の通常ポート検索（PK_HANDLER→BLK_ON_HANDLER）に吸収 */
        else {
            char nm[CFG_MAX_NAME];
            int pi;
            strcpy(nm, P.cur.text); adv();
            if (P.cur.type == T_GT || P.cur.type == T_LT || P.cur.type == T_GE ||
                P.cur.type == T_LE || P.cur.type == T_EQ || P.cur.type == T_NE)
                fail(line, ERR_SYNTAX);   /* ON <条件> エッジは未対応 */
            pi = vm_find_port(nm);
            if (pi < 0) fail_tok(line, ERR_UNKNOWN_NAME, nm);          /* 未登録のON源 */
            if (vm()->ports[pi].kind != PK_HANDLER)
                fail_tok(line, ERR_SYNTAX, nm);                       /* def_handlerでない（役割違反） */
            kind = BLK_ON_HANDLER; handler = pi;
        }
    } else {
        fail(line, ERR_SYNTAX);   /* 不正なONトリガ */
    }

    expect_newline();
    {
        int bi = add_block(kind);
        int amark;
        vm()->blocks[bi].period   = period;
        vm()->blocks[bi].handler_port = handler;
        P.yield_ok = 0;   /* ON ハンドラは WAIT 禁止 */
        P.nest = 0;
        P.repeat_depth = 0;
        P.cur_port_type = -1; P.cur_port_sidx = -1;   /* ハンドラ本体（スクリプト内ポートではない, v0.4.5） */
        amark = s_nalias;           /* ローカル別名スコープ入口（v0.4.6） */
        parse_local_decls();
        parse_stmt_list();
        if (!is_kw("END")) fail_end(P.cur.line, P.open_line);  /* aux=ONの開きヘッダ行 */
        adv();
        emit8(OP_HALT);
        s_nalias = amark;           /* ブロック局所別名を捨てる（v0.4.6） */
    }
    expect_newline();
}

/* def_* 行（C側マクロの写し, §13）は読み飛ばす */
static void skip_def_line(void)
{
    while (P.cur.type != T_NEWLINE && P.cur.type != T_EOF) adv();
    if (P.cur.type == T_NEWLINE) adv();
}

/* def_alias(NAME, target) を解析して表に登録（v0.3.8）。target は GVAR[k]/SGVAR[k]/数値/文字列。
 * 他の def_* と違い、コンパイラが実際に解釈する（ホスト相棒の無いスクリプト内部定義）。 */
static void parse_def_alias(void)
{
    char name[CFG_MAX_NAME];
    int line = P.cur.line;
    alias_t *al;
    adv();                                  /* "def_alias" を消費 */
    expect(T_LPAREN, "'('");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
    strcpy(name, P.cur.text);
    adv();
    if (is_reserved_name(name) || vm_find_port(name) >= 0 || alias_find(name))
        fail_tok(line, ERR_SYNTAX, name);   /* 予約語/既登録ポート・定数/重複と衝突 */
    expect(T_COMMA, "','");
    if (s_nalias >= CFG_MAX_ALIAS) fail(line, ERR_SYNTAX);   /* 表が満杯（資源限界） */
    al = &s_aliases[s_nalias];
    strncpy(al->name, name, CFG_MAX_NAME - 1); al->name[CFG_MAX_NAME - 1] = '\0';
    if      (P.cur.type == T_NUMBER) { al->kind = AL_INT; al->v = P.cur.num;     adv(); }
    else if (P.cur.type == T_STRING) { al->kind = AL_STR; al->v = P.cur.str_off; adv(); }  /* リテラルは既にプール1回intern済 */
    else if (is_kw("GVAR"))  { adv(); al->kind = AL_GVAR;  al->v = read_index(CFG_GVAR_COUNT,  "GVAR"); }
    else if (is_kw("SGVAR")) { adv(); al->kind = AL_SGVAR; al->v = read_index(CFG_SGVAR_COUNT, "SGVAR"); }
    else fail(P.cur.line, ERR_SYNTAX);      /* 対象は GVAR/SGVAR/数値/文字列 のみ（VAR等は不可） */
    s_nalias++;
    expect(T_RPAREN, "')'");
    expect_newline();
}

/* def_local(NAME, 実体) を解析してブロック局所エイリアスを表に追加（v0.4.6）。実体は
 * VAR/SVAR（読み書き）・ARG/SARG（読取専用）・数値/文字列（読専）。GVAR/SGVAR は不可（def_alias で）。
 * ブロック冒頭でのみ呼ばれ、END でブロック parser が s_nalias を切り詰めて局所分を捨てる。
 * 大域エイリアス名との衝突／同一ブロック内二重宣言は alias_find が拾って ERR_SYNTAX。 */
static void parse_def_local(void)
{
    char name[CFG_MAX_NAME];
    int line = P.cur.line;
    alias_t *al;
    adv();                                  /* "def_local" を消費 */
    expect(T_LPAREN, "'('");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
    strncpy(name, P.cur.text, CFG_MAX_NAME - 1); name[CFG_MAX_NAME - 1] = '\0';
    adv();
    /* 衝突: 予約語・スロット名・登録ポート／大域or同一ブロックの既存エイリアス（別ブロック同名は切り詰め済で不在） */
    if (is_reserved_name(name) || vm_find_port(name) >= 0 || alias_find(name))
        fail_tok(line, ERR_SYNTAX, name);
    expect(T_COMMA, "','");
    if (s_nalias >= CFG_MAX_ALIAS) fail(line, ERR_SYNTAX);   /* 表が満杯 */
    al = &s_aliases[s_nalias];
    strncpy(al->name, name, CFG_MAX_NAME - 1); al->name[CFG_MAX_NAME - 1] = '\0';
    if      (P.cur.type == T_NUMBER) { al->kind = AL_INT;  al->v = P.cur.num;     adv(); }
    else if (P.cur.type == T_STRING) { al->kind = AL_STR;  al->v = P.cur.str_off; adv(); }
    else if (is_kw("VAR"))  { adv(); al->kind = AL_VAR;  al->v = read_index(CFG_VAR_COUNT,  "VAR"); }
    else if (is_kw("SVAR")) { adv(); al->kind = AL_SVAR; al->v = read_index(CFG_SVAR_COUNT, "SVAR"); }
    else if (is_kw("ARG"))  { adv(); al->kind = AL_ARG;  al->v = read_index(CFG_ARG_COUNT,  "ARG"); }
    else if (is_kw("SARG")) { adv(); al->kind = AL_SARG; al->v = read_index(CFG_SARG_COUNT, "SARG"); }
    else fail(P.cur.line, ERR_SYNTAX);      /* 対象は VAR/SVAR/ARG/SARG/数値/文字列 のみ（GVAR/SGVARは def_alias） */
    s_nalias++;
    expect(T_RPAREN, "')'");
    expect_newline();
}

/* ブロック冒頭の def_local 宣言列を消費（本体の最初の文より前・一括, v0.4.6）。 */
static void parse_local_decls(void)
{
    for (;;) {
        skip_newlines();
        if (!is_kw("def_local")) return;
        parse_def_local();
    }
}

/* def_handler(NAME) を解析してハンドラ源ポートを登録（v0.4.1）。
 * handler は束縛すべきCの実体（関数/値）を持たない＝名前だけのイベントチャネルなので、
 * def_alias に続きコンパイラが実際に解釈する2つ目の def_。意味は「存在を保証する（冪等）」:
 * C側が同名を登録済みなら no-op、無ければ新規に生やす。以後 ON NAME / 値 -> NAME が解決する。 */
static void parse_def_handler(void)
{
    char name[CFG_MAX_NAME];
    int line = P.cur.line, pi;
    adv();                                  /* "def_handler" を消費 */
    expect(T_LPAREN, "'('");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
    strncpy(name, P.cur.text, CFG_MAX_NAME - 1); name[CFG_MAX_NAME - 1] = '\0';
    adv();
    expect(T_RPAREN, "')'");
    expect_newline();

    /* 衝突: 予約語・スロット名・制御語／別名と同名は不可 */
    if (is_reserved_name(name) || alias_find(name)) fail_tok(line, ERR_SYNTAX, name);
    pi = vm_find_port(name);
    if (pi >= 0) {
        if (vm()->ports[pi].kind == PK_HANDLER) return;   /* 既にハンドラ源＝冪等 no-op */
        fail_tok(line, ERR_SYNTAX, name);                 /* 既登録の非handlerポート/定数と衝突 */
    }
    /* 新規登録。ポート表満杯なら add_port は登録せず（黙ってNULL）→ 見つからないので検出 */
    script_register_handler(name);
    if (vm_find_port(name) < 0) fail_tok(line, ERR_TOO_MANY_PORTS, name);
}

/* def_port(NAME, T_INT|T_STR) を解析してスクリプト内ポートを登録（v0.4.5）。本体は PORT ブロック
 * （後で bc_start を後埋め）。def_alias/def_handler に続きコンパイラが解釈する3つ目の def_。
 * 宣言は冒頭一括・使用/本体より前。sidx 表にも積んで呼び出しグラフ判定（DFS）の対象にする。 */
static void parse_def_port(void)
{
    char name[CFG_MAX_NAME];
    int line = P.cur.line, pi;
    script_type_t ot;
    adv();                                  /* "def_port" を消費 */
    expect(T_LPAREN, "'('");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
    strncpy(name, P.cur.text, CFG_MAX_NAME - 1); name[CFG_MAX_NAME - 1] = '\0';
    adv();
    expect(T_COMMA, "','");
    if      (is_kw("T_INT")) ot = SCRIPT_T_INT;
    else if (is_kw("T_STR")) ot = SCRIPT_T_STR;
    else { fail(P.cur.line, ERR_SYNTAX); return; }   /* 産出型は T_INT / T_STR のみ */
    adv();
    expect(T_RPAREN, "')'");
    expect_newline();

    /* 衝突: 予約語・スロット名・別名は不可。既登録の非scriptポートとの同名も不可（§3）。
     * 既登録の PK_SCRIPT との同名は冪等（再宣言OK＝本体は毎ロード PORT ブロックで張り直す。
     * ポート表はロード間で残るため、再ロードでの再宣言を許す必要がある）。 */
    if (is_reserved_name(name) || alias_find(name)) fail_tok(line, ERR_SYNTAX, name);
    pi = vm_find_port(name);
    if (pi >= 0 && vm()->ports[pi].kind != PK_SCRIPT) fail_tok(line, ERR_SYNTAX, name);
    if (pi >= 0 && script_sidx_of(pi) >= 0) fail_tok(line, ERR_SYNTAX, name);  /* 同一スクリプト内の二重宣言 */
    if (s_nscript_ports >= CFG_MAX_SCRIPT_PORTS) fail_tok(line, ERR_TOO_MANY_PORTS, name);
    script_register_script_port(name, ot);            /* 既存なら上書き再束縛・bc_start=0xFFFF（本体未定義） */
    pi = vm_find_port(name);
    if (pi < 0) fail_tok(line, ERR_TOO_MANY_PORTS, name);  /* ポート表満杯 */
    s_script_ports[s_nscript_ports] = pi;
    s_call_edges[s_nscript_ports]   = 0;
    s_nscript_ports++;
}

/* PORT NAME … END（スクリプト内ポート本体, v0.4.5）。NAME は def_port 済みで本体未定義であること。
 * bc_start をここで確定し、本体を yield不可（WAIT=ERR_WAIT_IN_ON）でコンパイル。EXIT の型検査に
 * cur_port_type、呼び出しグラフの辺記録に cur_port_sidx を張る。フォールスルーで既定値を産出。 */
static void parse_port_block(void)
{
    char name[CFG_MAX_NAME];
    int line = P.open_line, pi, sidx, amark;
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
    strncpy(name, P.cur.text, CFG_MAX_NAME - 1); name[CFG_MAX_NAME - 1] = '\0';
    adv();
    pi = vm_find_port(name);
    if (pi < 0 || vm()->ports[pi].kind != PK_SCRIPT) fail_tok(line, ERR_SYNTAX, name);  /* def_port 宣言が無い */
    if (vm()->ports[pi].bc_start != 0xFFFF)          fail_tok(line, ERR_SYNTAX, name);  /* 本体二重定義 */
    sidx = script_sidx_of(pi);
    expect_newline();
    vm()->ports[pi].bc_start = (uint16_t)here();      /* 本体開始を確定（前方参照はこの番号で解決） */
    P.yield_ok = 0;             /* WAIT 不可（ERR_WAIT_IN_ON・within-tick） */
    P.nest = 0;
    P.repeat_depth = 0;
    P.cur_port_type = (vm()->ports[pi].out_type == SCRIPT_T_STR) ? TY_STR : TY_INT;
    P.cur_port_sidx = sidx;
    amark = s_nalias;           /* ローカル別名スコープ入口（v0.4.6） */
    parse_local_decls();
    parse_stmt_list();
    if (!is_kw("END")) fail_end(P.cur.line, P.open_line);
    adv();
    emit_exit_default(P.cur_port_type);   /* EXIT されず END 到達＝既定値(0/空)を産出して return */
    s_nalias = amark;           /* ブロック局所別名を捨てる（v0.4.6） */
    P.cur_port_type = -1;
    P.cur_port_sidx = -1;
    expect_newline();
}

/* 呼び出しグラフの DFS（§3 v0.4.5）。gray-node のバックエッジでサイクル検出、同時に
 * depth[node]=1+max(depth[子]) で最長路＝最大コールスタック段数を算出。戻り値<0＝サイクル。 */
static int dfs_depth(int sidx)
{
    int j, best = 0;
    if (s_dfs_color[sidx] == 1) { s_cycle_pi = s_script_ports[sidx]; return -1; }  /* バックエッジ */
    if (s_dfs_color[sidx] == 2) return s_dfs_memo[sidx];
    s_dfs_color[sidx] = 1;
    for (j = 0; j < s_nscript_ports; j++) {
        if (s_call_edges[sidx] & (1u << j)) {
            int d = dfs_depth(j);
            if (d < 0) { if (s_cycle_pi < 0) s_cycle_pi = s_script_ports[sidx]; return -1; }
            if (d > best) best = d;
        }
    }
    s_dfs_color[sidx] = 2;
    s_dfs_memo[sidx]  = best + 1;
    return best + 1;
}

/* プログラム状態をロード前にリセット（§4：GVAR/VAR 0クリア） */
static void reset_program(void)
{
    script_vm_t *m = vm();
    /* 注: スクリプト def_handler で増えたポートは reset_program では消さない。ポート表の寿命は
     * script_init（§3 v0.4.1）。現行ホスト（PC/STM32ローダ）は各ロードで再initするので前ロードの
     * 宣言は残らない。※単一initで複数スクリプトを compile する場合は def_handler ポートが積み増さ
     * れる（冪等なので同名再宣言は no-op・CFG_MAX_PORTS の予算内で運用すること）。 */
    m->code_len = 0;
    m->strpool_len = 0;
    m->nblocks = 0;
    m->init_blk = -1;
    m->main_blk = -1;
    memset(m->gvar, 0, sizeof(m->gvar));
    memset(m->var,  0, sizeof(m->var));
    memset(m->arg,  0, sizeof(m->arg));
    memset(m->sarg, 0, sizeof(m->sarg));
    m->result = val_int(0);
    memset(m->sgvar, 0, sizeof(m->sgvar));   /* 文字列スロットは空文字でクリア（§4） */
    memset(m->svar,  0, sizeof(m->svar));
    memset(m->sresult, 0, sizeof(m->sresult));
    m->status = 0;
    m->sp = 0;
    m->loop_sp = 0;   /* ループフレーム（§7 REPEAT, v0.4.4） */
    m->call_sp = 0;   /* コールフレーム（§3 §7 スクリプト内ポート, v0.4.5） */
    /* スクリプト内ポートの本体は毎ロードで再定義（前ロードの bc_start を無効化＝未定義に戻す）。
     * ポート表自体は残るが、本体は PORT ブロックで張り直す。呼び出しグラフ表も総入れ替え。 */
    {
        int i;
        for (i = 0; i < m->nports; i++)
            if (m->ports[i].kind == PK_SCRIPT) m->ports[i].bc_start = 0xFFFF;
    }
    s_nscript_ports = 0;
    memset(&m->main_ctx, 0, sizeof(m->main_ctx));
    m->loaded = false;
    m->init_done = false;
    m->evq_head = m->evq_tail = 0;
    m->evq_overflow = false;
    memset(m->timers, 0, sizeof(m->timers));
    m->timer_overflow = false;
    s_nalias = 0;   /* エイリアス表もロードごとにクリア（v0.3.8） */
}

static void top_level_unexpected(void)   /* 先頭で INIT/MAIN/ON/def_* 以外（v0.3.7） */
{
    if (is_kw("END")) fail(P.cur.line, ERR_UNEXPECTED_END);   /* 対応ヘッダの無い END */
    fail(P.cur.line, ERR_SYNTAX);
}

int compiler_compile(const char *src, size_t len)
{
    script_vm_t *m = vm();
    memset(&s_error, 0, sizeof(s_error));   /* 直近エラーをクリア */
    if (!m) { s_error.code = ERR_SYNTAX; return ERR_SYNTAX; }   /* script_init 未呼び出し */

    reset_program();
    memset(&P, 0, sizeof(P));
    P.cur_port_type = -1; P.cur_port_sidx = -1;   /* 既定＝非ポート（memset の 0 は有効値なので明示, v0.4.5） */
    lex_init(&P.lx, src, len);

    if (setjmp(P.jb)) {                 /* エラー巻き戻し（s_error は fail_e が設定済み） */
        m->loaded = false;
        return (int)s_error.code;       /* 非0 = ロードエラー（v0.3.7） */
    }

    adv();
    for (;;) {
        skip_newlines();
        if (P.cur.type == T_EOF) break;
        if (P.cur.type != T_IDENT) top_level_unexpected();

        if (is_kw("def_alias"))   { parse_def_alias();   continue; }   /* コンパイラが解釈する def_（v0.3.8） */
        if (is_kw("def_handler")) { parse_def_handler(); continue; }   /* 同上・ハンドラ源宣言（v0.4.1） */
        if (is_kw("def_port"))    { parse_def_port();    continue; }   /* 同上・スクリプト内ポート宣言（v0.4.5） */
        if (is_kw("def_local"))   fail(P.cur.line, ERR_SYNTAX);        /* def_local はブロック冒頭のみ（ファイル冒頭不可, v0.4.6） */
        if (!strncmp(P.cur.text, "def_", 4)) { skip_def_line(); continue; }  /* 他の def_ は非解釈（写し） */
        if (is_kw("INIT")) { P.open_line = P.cur.line; adv(); parse_simple_block(BLK_INIT); }
        else if (is_kw("MAIN")) { P.open_line = P.cur.line; adv(); parse_simple_block(BLK_MAIN); }
        else if (is_kw("ON")) { P.open_line = P.cur.line; adv(); parse_on_block(); }
        else if (is_kw("PORT")) { P.open_line = P.cur.line; adv(); parse_port_block(); }   /* スクリプト内ポート本体（v0.4.5） */
        else top_level_unexpected();
    }

    /* スクリプト内ポート: 本体存在チェック＋呼び出しグラフ判定（§3 v0.4.5・全ブロックのコンパイル後）。 */
    {
        int i, maxdepth = 0;
        for (i = 0; i < s_nscript_ports; i++)
            if (m->ports[s_script_ports[i]].bc_start == 0xFFFF)
                fail_tok(0, ERR_SYNTAX, m->ports[s_script_ports[i]].name);  /* def_port 宣言のみ・本体なし */
        for (i = 0; i < s_nscript_ports; i++) { s_dfs_color[i] = 0; s_dfs_memo[i] = 0; }
        s_cycle_pi = -1;
        for (i = 0; i < s_nscript_ports; i++) {
            int d = dfs_depth(i);
            if (d < 0) fail_tok(0, ERR_RECURSION, s_cycle_pi >= 0 ? m->ports[s_cycle_pi].name : "");
            if (d > maxdepth) maxdepth = d;
        }
        if (maxdepth > CFG_CALL_NEST) fail(0, ERR_NEST_TOO_DEEP);   /* コールスタック段数の静的検算 */
    }

    /* 実行コンテキスト確定。ロード直後は INITフェーズに入る（v0.3.4）。
     * 中断コンテキスト(main_ctx)はまず INIT を保持し、INIT→RUN 切替で MAIN へ張り替わる。 */
    m->loaded = true;
    m->init_done = false;                  /* INITフェーズ（再ロードも毎回ここから） */
    if (m->init_blk >= 0) {
        m->main_ctx.started = true;
        m->main_ctx.pc = m->blocks[m->init_blk].bc_start;
        m->main_ctx.waiting = false;
        m->main_ctx.wake_time = 0;
    } else {
        m->main_ctx.started = false;       /* INIT無し → 最初のtickで即RUNへ */
    }
    s_error.code = ERR_NONE;
    return 0;
}

const script_error_t *compiler_last_error(void)
{
    return &s_error;
}
