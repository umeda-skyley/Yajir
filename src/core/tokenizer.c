/* tokenizer.c - 字句解析の実装（仕様 §2, §9） */
#include <string.h>
#include <stdio.h>
#include "tokenizer.h"
#include "vm.h"

/* 行末（改行デリミタ）判定（§2, CFG_LINE_DELIM）。
 *   LX_IS_NEWLINE : 行末トークン T_NEWLINE を生む文字。
 *   LX_IS_NL_SPACE: 行末にせず空白として読み飛ばす CR/LF（厳密モードの副改行）。
 * 既定（非厳密）は CR・LF 双方を行末として受理（CRLF は下で1改行に畳む）。 */
#if CFG_LINE_DELIM_STRICT
#  define LX_IS_NEWLINE(c)   ((c) == CFG_LINE_DELIM)
#  define LX_IS_NL_SPACE(c)  (((c) == '\r' || (c) == '\n') && (c) != CFG_LINE_DELIM)
#else
#  define LX_IS_NEWLINE(c)   ((c) == '\r' || (c) == '\n')
#  define LX_IS_NL_SPACE(c)  (0)
#endif

void lex_init(lexer_t *lx, const char *src, size_t len)
{
    lx->src = src;
    lx->len = len;
    lx->pos = 0;
    lx->line = 1;
}

static int at_end(lexer_t *lx) { return lx->pos >= lx->len; }
static char peek(lexer_t *lx)  { return at_end(lx) ? '\0' : lx->src[lx->pos]; }
static char peek2(lexer_t *lx) { return (lx->pos + 1 >= lx->len) ? '\0' : lx->src[lx->pos + 1]; }

static int is_ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static int is_ident(char c)       { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(char c)       { return c >= '0' && c <= '9'; }

static int lex_err(char *errbuf, size_t sz, int *errline, int line, const char *msg)
{
    if (errbuf && sz) { strncpy(errbuf, msg, sz - 1); errbuf[sz - 1] = '\0'; }
    if (errline) *errline = line;
    return -1;
}

/* 文字列定数を読み、エスケープ処理して文字列プールへインターンする（§2, §9） */
static int lex_string(lexer_t *lx, token_t *out, char *errbuf, size_t errbuf_sz, int *errline)
{
    script_vm_t *m = vm();
    int start_off = m->strpool_len;
    lx->pos++; /* 開始の " を消費 */
    while (!at_end(lx) && peek(lx) != '"') {
        char c = peek(lx);
        if (c == '\n' || c == '\r') return lex_err(errbuf, errbuf_sz, errline, lx->line, "unterminated string");  /* 文字列は1行内 */
        if (c == '\\') {
            char e = peek2(lx);
            switch (e) {
                case 'r':  c = '\r'; break;
                case 'n':  c = '\n'; break;
                case 't':  c = '\t'; break;
                case '\\': c = '\\'; break;
                case '"':  c = '"';  break;
                default:
                    return lex_err(errbuf, errbuf_sz, errline, lx->line, "bad escape sequence");
            }
            lx->pos += 2;
        } else {
            lx->pos++;
        }
        if (m->strpool_len + 1 >= CFG_STRPOOL_SIZE)
            return lex_err(errbuf, errbuf_sz, errline, lx->line, "string pool overflow");
        m->strpool[m->strpool_len++] = c;
    }
    if (at_end(lx)) return lex_err(errbuf, errbuf_sz, errline, lx->line, "unterminated string");
    lx->pos++; /* 終端の " を消費 */
    if (m->strpool_len + 1 >= CFG_STRPOOL_SIZE)
        return lex_err(errbuf, errbuf_sz, errline, lx->line, "string pool overflow");
    m->strpool[m->strpool_len++] = '\0';
    out->type = T_STRING;
    out->str_off = start_off;
    return 0;
}

int lex_next(lexer_t *lx, token_t *out, char *errbuf, size_t errbuf_sz, int *errline)
{
    memset(out, 0, sizeof(*out));

    /* 空白・コメントをスキップ（行末デリミタは残す） */
    for (;;) {
        char c = peek(lx);
        if (c == ' ' || c == '\t' || LX_IS_NL_SPACE(c)) { lx->pos++; continue; }
        if (c == '/' && peek2(lx) == '/') {           /* 行コメント（CR/LF どちらでも終端） */
            while (!at_end(lx) && peek(lx) != '\n' && peek(lx) != '\r') lx->pos++;
            continue;
        }
        break;
    }

    out->line = lx->line;

    if (at_end(lx)) { out->type = T_EOF; return 0; }

    char c = peek(lx);

    if (LX_IS_NEWLINE(c)) {
        lx->pos++;                                 /* 改行1文字目を消費 */
        { char d = peek(lx);                       /* CRLF / LFCR は1改行に畳む（行番号の二重計上を防ぐ） */
          if ((c == '\r' && d == '\n') || (c == '\n' && d == '\r')) lx->pos++; }
        lx->line++;
        out->type = T_NEWLINE;
        return 0;
    }

    if (c == '"') return lex_string(lx, out, errbuf, errbuf_sz, errline);

    if (is_digit(c)) {
        /* 10進/16進(0x)/2進(0b)。'_' は桁区切りで読み飛ばし。33bit以上はエラー（§2） */
        uint64_t v = 0;
        int got = 0;
        if (c == '0' && (peek2(lx) == 'x' || peek2(lx) == 'X')) {
            lx->pos += 2;
            for (;;) {
                char d = peek(lx);
                int hv;
                if (d == '_') { lx->pos++; continue; }
                if (d >= '0' && d <= '9') hv = d - '0';
                else if (d >= 'a' && d <= 'f') hv = d - 'a' + 10;
                else if (d >= 'A' && d <= 'F') hv = d - 'A' + 10;
                else break;
                v = v * 16 + hv; got++; lx->pos++;
                if (v > 0xFFFFFFFFull) return lex_err(errbuf, errbuf_sz, errline, lx->line, "integer literal exceeds 32 bits");
            }
            if (!got) return lex_err(errbuf, errbuf_sz, errline, lx->line, "malformed hex literal");
        } else if (c == '0' && (peek2(lx) == 'b' || peek2(lx) == 'B')) {
            lx->pos += 2;
            for (;;) {
                char d = peek(lx);
                if (d == '_') { lx->pos++; continue; }
                if (d != '0' && d != '1') break;
                v = v * 2 + (d - '0'); got++; lx->pos++;
                if (v > 0xFFFFFFFFull) return lex_err(errbuf, errbuf_sz, errline, lx->line, "integer literal exceeds 32 bits");
            }
            if (!got) return lex_err(errbuf, errbuf_sz, errline, lx->line, "malformed binary literal");
        } else {
            for (;;) {
                char d = peek(lx);
                if (d == '_') { lx->pos++; continue; }
                if (!is_digit(d)) break;
                v = v * 10 + (d - '0'); lx->pos++;
                if (v > 0xFFFFFFFFull) return lex_err(errbuf, errbuf_sz, errline, lx->line, "integer literal exceeds 32 bits");
            }
        }
        out->type = T_NUMBER;
        out->num = (int32_t)(uint32_t)v;   /* int32 のビットパターンとして格納（§2） */
        return 0;
    }

    if (is_ident_start(c)) {
        int n = 0;
        while (is_ident(peek(lx))) {
            if (n < CFG_MAX_NAME - 1) out->text[n++] = peek(lx);
            lx->pos++;
        }
        out->text[n] = '\0';
        out->type = T_IDENT;
        return 0;
    }

    /* 記号・演算子 */
    lx->pos++;
    switch (c) {
        case ',': out->type = T_COMMA;    return 0;
        case '(': out->type = T_LPAREN;   return 0;
        case ')': out->type = T_RPAREN;   return 0;
        case '[': out->type = T_LBRACKET; return 0;
        case ']': out->type = T_RBRACKET; return 0;
        case '+': out->type = T_PLUS;     return 0;
        case '*': out->type = T_STAR;     return 0;
        case '/': out->type = T_SLASH;    return 0;   /* // 行コメントは上でスキップ済み */
        case '%': out->type = T_PCT;      return 0;
        case '&': out->type = T_AMP;      return 0;
        case '|': out->type = T_PIPE;     return 0;
        case '^': out->type = T_CARET;    return 0;
        case '~': out->type = T_TILDE;    return 0;
        case '-':
            if (peek(lx) == '>') { lx->pos++; out->type = T_ARROW; return 0; }
            out->type = T_MINUS; return 0;
        case '>':
            if (peek(lx) == '>') { lx->pos++; out->type = T_SHR; return 0; }
            if (peek(lx) == '=') { lx->pos++; out->type = T_GE;  return 0; }
            out->type = T_GT; return 0;
        case '<':
            if (peek(lx) == '<') { lx->pos++; out->type = T_SHL; return 0; }
            if (peek(lx) == '=') { lx->pos++; out->type = T_LE;  return 0; }
            out->type = T_LT; return 0;
        case '=':
            if (peek(lx) == '=') { lx->pos++; out->type = T_EQ; return 0; }
            return lex_err(errbuf, errbuf_sz, errline, lx->line, "unexpected '=' (did you mean '=='?)");
        case '!':
            if (peek(lx) == '=') { lx->pos++; out->type = T_NE; return 0; }
            return lex_err(errbuf, errbuf_sz, errline, lx->line, "unexpected '!' (did you mean '!='?)");
        default: {
            char msg[40];
            snprintf(msg, sizeof(msg), "unexpected character '%c'", c);
            return lex_err(errbuf, errbuf_sz, errline, lx->line, msg);
        }
    }
}
