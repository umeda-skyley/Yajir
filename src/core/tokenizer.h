/* tokenizer.h - 字句解析（仕様 §2）
 *
 * 1行1文。行コメント //。識別子は大文字推奨だが本実装は大小区別あり（§16ノブはdefault区別）。
 * 文字列定数はエスケープ \r \n \t \\ \" をサポートし、解析時に文字列プールへインターンする。
 *
 * ホスト非依存（純C）コア層。
 */
#ifndef TOKENIZER_H
#define TOKENIZER_H

#include <stddef.h>
#include <stdint.h>
#include "script_config.h"

typedef enum {
    T_EOF = 0,
    T_NEWLINE,     /* 文の区切り */
    T_NUMBER,      /* 整数リテラル → num */
    T_STRING,      /* 文字列定数  → str_off（プールオフセット） */
    T_IDENT,       /* 識別子/予約語 → text */
    T_ARROW,       /* ->  */
    T_COMMA,       /* ,   */
    T_LPAREN,      /* (   */
    T_RPAREN,      /* )   */
    T_LBRACKET,    /* [   */
    T_RBRACKET,    /* ]   */
    T_PLUS,        /* +   */
    T_MINUS,       /* -   */
    T_STAR,        /* *   */
    T_SLASH,       /* /   */
    T_PCT,         /* %   */
    T_AMP,         /* &   */
    T_PIPE,        /* |   */
    T_CARET,       /* ^   */
    T_TILDE,       /* ~   */
    T_SHL,         /* <<  */
    T_SHR,         /* >>  */
    T_GT, T_LT, T_GE, T_LE, T_EQ, T_NE  /* > < >= <= == != */
} tok_type;

typedef struct {
    tok_type type;
    int      line;
    int32_t  num;                 /* T_NUMBER */
    int32_t  str_off;             /* T_STRING：文字列プール内オフセット */
    char     text[CFG_MAX_NAME];  /* T_IDENT */
} token_t;

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    int         line;
} lexer_t;

void lex_init(lexer_t *lx, const char *src, size_t len);

/* 次のトークンを取り出す。0=ok / <0=字句エラー（errbuf/errlineに設定） */
int  lex_next(lexer_t *lx, token_t *out, char *errbuf, size_t errbuf_sz, int *errline);

#endif /* TOKENIZER_H */
