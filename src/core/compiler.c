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
    int      open_line;   /* 直近に開いたブロック/IFYESのヘッダ行（END欠落の aux 用, v0.3.7） */
    jmp_buf  jb;
} parser_t;

static parser_t P;
static script_error_t s_error;   /* 直近のロード失敗（構造化, v0.3.7 §11） */

/* ---- エイリアス（def_alias, v0.3.8）。純コンパイル時の名前解決＝VM は一切関与しない ----
 * 対象は GVAR/SGVAR（読み書き可）・数値・文字列（読取専用）。VAR/SVAR/ARG/SARG は不可
 * （意味が局所/プロトコル位置依存ゆえ安定した名前を付けられない）。 */
typedef enum { AL_GVAR, AL_SGVAR, AL_INT, AL_STR } alias_kind_t;
typedef struct {
    char         name[CFG_MAX_NAME];
    alias_kind_t kind;
    int32_t      v;   /* GVAR/SGVAR=添字 / INT=値 / STR=文字列プールoffset */
} alias_t;
static alias_t s_aliases[CFG_MAX_ALIAS];
static int     s_nalias;

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
        "INIT","MAIN","ON","END","ELSE", 0
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
static int g_str_reads;   /* この expr_list 中の str産出ポート読みの数（単一SRESULT衝突回避, v0.3.8） */

static void need_int(int line)
{
    if (g_type != TY_INT)
        fail(line, ERR_TYPE_MISMATCH);   /* 式中で文字列（算術/比較は int, 文字列は EQUALS） */
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
    if (is_kw("none")) { adv(); g_first_type = TY_INT; return 0; }
    parse_expr(); n = 1; g_first_type = g_type;
    while (P.cur.type == T_COMMA) { adv(); parse_expr(); n++; }
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

static void parse_stmt(void)
{
    int line = P.cur.line;
    int argc = parse_expr_list();
    char tname[CFG_MAX_NAME];
    int pi;

    expect(T_ARROW, "'->'");
    if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);   /* -> の後に送り先が無い */
    strcpy(tname, P.cur.text);
    adv();

    /* パイプライン・チェイン（§5, v0.4）：直後が '->' のセグメントは中間段＝inout のみ。
     * 「前段を呼ぶ → その産出値(int=RESULT/str=SRESULT)を次段の暗黙左辺へ1個LOAD」を繰り返し、
     * '->' で続かないセグメント（＝終端）で下の既存処理に合流する。純コンパイル時糖衣＝新opなし。 */
    while (P.cur.type == T_ARROW) {
        pi = vm_find_port(tname);
        if (pi < 0) fail_tok(line, ERR_UNKNOWN_PORT, tname);
        if (vm()->ports[pi].kind != PK_INOUT)
            fail_tok(line, ERR_BAD_POSITION, tname);    /* out/handler/in/const を中間に置けない */
        emit8(OP_CALL_OUT); emit8(pi); emit8(argc);     /* 前段を呼ぶ（先頭はargc, 継ぎ目以降は1） */
        if (vm()->ports[pi].out_type == SCRIPT_T_STR) { emit8(OP_LOAD_SRESULT); g_first_type = TY_STR; }
        else                                          { emit8(OP_LOAD_RESULT);  g_first_type = TY_INT; }
        argc = 1;                                       /* 継ぎ目は常に単一値（純パイプ・§5） */
        adv();                                          /* '->' を消費 */
        if (P.cur.type != T_IDENT) fail(P.cur.line, ERR_SYNTAX);
        strcpy(tname, P.cur.text);
        adv();
    }

    /* ---- 終端段（tname）---- */
    /* 制御ポート（被送信値は int） */
    if (!strcmp(tname, "IFYES")) {
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);   /* 条件は int（文字列はEQUALS） */
        parse_if(argc, line); return;
    }
    if (!strcmp(tname, "WAIT")) {
        if (!P.yield_ok) fail(line, ERR_WAIT_IN_ON);                 /* ON内のWAIT（§7） */
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_YIELD); expect_newline(); return;
    }
    if (!strcmp(tname, "TIMER")) {
        if (g_first_type != TY_INT) fail(line, ERR_TYPE_MISMATCH);
        normalize_to_one(argc); emit8(OP_ARM_TIMER); expect_newline(); return;
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
    int bi;
    expect_newline();           /* ヘッダ行終端 */
    bi = add_block(kind);
    P.yield_ok = (kind == BLK_MAIN || kind == BLK_INIT);  /* WAITはMAIN/INITで可（v0.3.4） */
    P.nest = 0;
    parse_stmt_list();
    if (!is_kw("END")) fail_end(P.cur.line, P.open_line);  /* aux=このブロックの開きヘッダ行 */
    adv();
    emit8(OP_HALT);
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
        vm()->blocks[bi].period   = period;
        vm()->blocks[bi].handler_port = handler;
        P.yield_ok = 0;   /* ON ハンドラは WAIT 禁止 */
        P.nest = 0;
        parse_stmt_list();
        if (!is_kw("END")) fail_end(P.cur.line, P.open_line);  /* aux=ONの開きヘッダ行 */
        adv();
        emit8(OP_HALT);
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

/* プログラム状態をロード前にリセット（§4：GVAR/VAR 0クリア） */
static void reset_program(void)
{
    script_vm_t *m = vm();
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

        if (is_kw("def_alias")) { parse_def_alias(); continue; }   /* コンパイラが解釈する唯一の def_（v0.3.8） */
        if (!strncmp(P.cur.text, "def_", 4)) { skip_def_line(); continue; }
        if (is_kw("INIT")) { P.open_line = P.cur.line; adv(); parse_simple_block(BLK_INIT); }
        else if (is_kw("MAIN")) { P.open_line = P.cur.line; adv(); parse_simple_block(BLK_MAIN); }
        else if (is_kw("ON")) { P.open_line = P.cur.line; adv(); parse_on_block(); }
        else top_level_unexpected();
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
