/* strutil.c - 標準装備Utilityポートの実装（仕様 §3） */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "strutil.h"
#include "script.h"
#include "vm.h"

#define STMP 256   /* 生成途中の作業バッファ。SRESULTへ書く際に最終切詰される */

/* buf に s を追記。あふれたら ERR_STR_TRUNC（§12） */
static void sb_append(char *buf, int cap, int *len, const char *s)
{
    for (; *s; s++) {
        if (*len < cap - 1) buf[(*len)++] = *s;
        else { vm_set_err(ERR_STR_TRUNC); break; }
    }
    buf[*len] = '\0';
}
static void sb_appendc(char *buf, int cap, int *len, char c)
{
    if (*len < cap - 1) { buf[(*len)++] = c; buf[*len] = '\0'; }
    else vm_set_err(ERR_STR_TRUNC);
}

/* SLICER: string, start(1始まり), len -> SRESULT（§3） */
static void th_slicer(int argc, const script_value_t *a)
{
    const char *s = (argc > 0) ? script_resolve_str(a[0]) : "";
    int start = (argc > 1) ? a[1].i : 1;
    int len   = (argc > 2) ? a[2].i : 0;
    int slen  = (int)strlen(s);
    int i0, avail, take, len_out = 0;
    char tmp[STMP];

    i0 = start - 1;                 /* 1始まり → 0始まり */
    if (i0 < 0) i0 = 0;
    if (i0 > slen) i0 = slen;
    avail = slen - i0;
    take = (len < 0) ? 0 : len;
    if (take > avail) take = avail;
    tmp[0] = '\0';
    { int k; for (k = 0; k < take; k++) sb_appendc(tmp, STMP, &len_out, s[i0 + k]); }
    script_set_sresult(tmp);
}

/* MERGER: strA, strB -> SRESULT（連結, §3） */
static void th_merger(int argc, const script_value_t *a)
{
    int len_out = 0;
    char tmp[STMP];
    tmp[0] = '\0';
    if (argc > 0) sb_append(tmp, STMP, &len_out, script_resolve_str(a[0]));
    if (argc > 1) sb_append(tmp, STMP, &len_out, script_resolve_str(a[1]));
    script_set_sresult(tmp);
}

/* COUNTER: string -> RESULT（バイト長, §3） */
static void th_counter(int argc, const script_value_t *a)
{
    const char *s = (argc > 0) ? script_resolve_str(a[0]) : "";
    script_set_result((int32_t)strlen(s));
}

/* EQUALS: strA, strB -> RESULT（一致1/不一致0, §3） */
static void th_equals(int argc, const script_value_t *a)
{
    const char *x = (argc > 0) ? script_resolve_str(a[0]) : "";
    const char *y = (argc > 1) ? script_resolve_str(a[1]) : "";
    script_set_result(strcmp(x, y) == 0 ? 1 : 0);
}

/* FINDER: hay, needle -> RESULT。needle の最初の出現位置（1始まり）。無ければ 0（POS/INSTR 相当, §3） */
static void th_finder(int argc, const script_value_t *a)
{
    const char *hay    = (argc > 0) ? script_resolve_str(a[0]) : "";
    const char *needle = (argc > 1) ? script_resolve_str(a[1]) : "";
    const char *p = strstr(hay, needle);
    script_set_result(p ? (int32_t)(p - hay) + 1 : 0);   /* 1始まり・無ければ0 */
}

/* STRTOL: str [, base] -> RESULT。文字列を整数にパースする（§3, v0.4 で標準採用）。
 * 既定の基数は 16（Wi-SUN 等のアドレス/16進応答をそのまま読むため＝既存スクリプト互換）。
 * 第2引数で基数を明示できる（2..36、0=Cの自動判定:0xで16進・0で8進・他10進）。
 * 引数無し/第1引数が非文字列 は型エラーとして RESULT=-1（ユーザー実装の規約を踏襲）。
 * パース不能な文字列は strtol 同様 0（先頭から読める所まで・無ければ0）。 */
static void th_strtol(int argc, const script_value_t *a)
{
    int base;
    if (argc < 1 || !script_val_is_str(a[0])) { script_set_result(-1); return; }
    base = (argc > 1) ? a[1].i : 16;
    if (base != 0 && (base < 2 || base > 36)) base = 16;   /* 不正基数は既定16へ丸め */
    script_set_result((int32_t)strtol(script_resolve_str(a[0]), NULL, base));
}

/* FIELD: str, [delim,] N -> SRESULT。区切りでN番目(1始まり)のトークンを取り出す（§3, v0.4）。
 *   "SEND 0001 0002 OK", " ", 3 -> FIELD  → "0002"   ／  line, 3 -> FIELD  ＝ 空白区切りの略記
 * delim は「区切り文字の集合」(strtok流)＝delim中のどの文字も区切り。連続区切りは潰し、
 * 前後の区切りは無視する（＝N番目の"単語"を取る挙動）。範囲外Nや空文字列は ""。
 * 可変長・多重スペースのモジュール応答を `FIELD -> STRTOL` で安全に分解できる。 */
static void th_field(int argc, const script_value_t *a)
{
    const char *s = (argc > 0) ? script_resolve_str(a[0]) : "";
    const char *delim;
    const char *p;
    int n, cur = 0, in_tok = 0, len_out = 0;
    char tmp[STMP];

    if (argc == 2) { delim = " \t"; n = a[1].i; }          /* 略記: 既定=空白区切り */
    else { delim = (argc > 1) ? script_resolve_str(a[1]) : " ";
           n = (argc > 2) ? a[2].i : 1; }
    tmp[0] = '\0';
    if (n < 1) { script_set_sresult(""); return; }

    for (p = s; ; p++) {
        int is_delim = (*p == '\0') ? 1 : (strchr(delim, *p) != NULL);  /* '\0'はstrchr前に確定 */
        if (is_delim) {
            if (in_tok) { in_tok = 0; if (cur == n) break; }  /* n番目のトークン末尾に到達 */
            if (*p == '\0') break;
        } else {
            if (!in_tok) { in_tok = 1; cur++; }               /* 新トークン開始 */
            if (cur == n) sb_appendc(tmp, STMP, &len_out, *p);
        }
    }
    script_set_sresult(tmp);   /* 見つからなければ tmp は "" のまま */
}

/* UPPER/LOWER: str -> SRESULT。ASCII大小変換（§3, v0.4）。プロトコル語の正規化用。 */
static void map_case(int argc, const script_value_t *a, int up)
{
    const char *s = (argc > 0) ? script_resolve_str(a[0]) : "";
    int len_out = 0;
    char tmp[STMP];
    tmp[0] = '\0';
    for (; *s; s++) {
        int c = (unsigned char)*s;
        sb_appendc(tmp, STMP, &len_out, (char)(up ? toupper(c) : tolower(c)));
    }
    script_set_sresult(tmp);
}
static void th_upper(int argc, const script_value_t *a) { map_case(argc, a, 1); }
static void th_lower(int argc, const script_value_t *a) { map_case(argc, a, 0); }

/* TRIMMER: str -> SRESULT。前後の空白類(空白/タブ/CR/LF等)を除去（§3, v0.4）。
 * UARTで1行取った際の末尾CR/LFや余白を落とす前処理に。中間の空白は保持。 */
static void th_trimmer(int argc, const script_value_t *a)
{
    const char *s = (argc > 0) ? script_resolve_str(a[0]) : "";
    const char *e;
    int len_out = 0;
    char tmp[STMP];
    tmp[0] = '\0';
    while (*s && isspace((unsigned char)*s)) s++;                /* 先頭側 */
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;          /* 末尾側 */
    for (; s < e; s++) sb_appendc(tmp, STMP, &len_out, *s);
    script_set_sresult(tmp);
}

/* FORMATTER: fmt, args... -> SRESULT。各変換指定をそのまま snprintf に委譲するので、
 * フラグ/幅/精度（"%4d" "%-10s" "%08x" "%+d" 等）がそのまま使える（§3, v0.3.8）。
 * コアが解釈するのは「変換文字の種別（int系 / str / %）」だけ＝対応する引数の型を選ぶため。
 * 危険な %n や未対応の %f 等は **書式として実行せず**そのまま文字列で出す（安全側）。 */
static void th_formatter(int argc, const script_value_t *a)
{
    const char *f = (argc > 0) ? script_resolve_str(a[0]) : "";
    int ai = 1;                      /* 次に消費する引数 */
    int len_out = 0;
    char tmp[STMP];
    char spec[24];                   /* 1変換指定（'%' …フラグ/幅/精度… 変換文字） */
    char piece[STMP];                /* snprintf 出力 */
    tmp[0] = '\0';

    while (*f) {
        if (*f != '%') { sb_appendc(tmp, STMP, &len_out, *f++); continue; }
        /* '%' から変換文字までを spec に収集。許可するのはフラグ/幅/精度のみ
         * （長さ修飾子 l/h 等と '*' は非対応＝Yajirはint32のみ）。 */
        {
            int si = 0; char conv;
            spec[si++] = *f++;                                   /* '%' */
            while (*f && si < (int)sizeof(spec) - 2 && strchr(" -+#0123456789.", *f))
                spec[si++] = *f++;
            if (!*f) { sb_appendc(tmp, STMP, &len_out, '%'); break; }  /* 末尾の裸 '%' */
            conv = *f++;
            spec[si++] = conv; spec[si] = '\0';

            if (conv == '%') { sb_appendc(tmp, STMP, &len_out, '%'); continue; }

            piece[0] = '\0';
            switch (conv) {
                case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c':
                    snprintf(piece, sizeof(piece), spec, (int)((ai < argc) ? a[ai].i : 0));
                    ai++; break;
                case 's':
                    snprintf(piece, sizeof(piece), spec, (ai < argc) ? script_resolve_str(a[ai]) : "");
                    ai++; break;
                default:   /* %f/%n 等：書式実行せず spec をそのまま出す（%n 無害化, §3） */
                    sb_append(tmp, STMP, &len_out, spec); continue;
            }
            sb_append(tmp, STMP, &len_out, piece);
        }
    }
    script_set_sresult(tmp);
}

void register_strutils(void)
{
    /* 値を返す関数ポート＝inout（産出型つき, v0.3.8）。get_fn は無し(NULL・送信専用)。
     * str産出（SRESULT）: SLICER/MERGER/FORMATTER ／ int産出（RESULT）: COUNTER/EQUALS。 */
    script_register_inout("SLICER",    NULL, th_slicer,    SCRIPT_T_STR);
    script_register_inout("MERGER",    NULL, th_merger,    SCRIPT_T_STR);
    script_register_inout("COUNTER",   NULL, th_counter,   SCRIPT_T_INT);
    script_register_inout("FORMATTER", NULL, th_formatter, SCRIPT_T_STR);
    script_register_inout("EQUALS",    NULL, th_equals,    SCRIPT_T_INT);
    script_register_inout("FINDER",    NULL, th_finder,    SCRIPT_T_INT);   /* 位置検索（1始まり）, v0.3.8 */
    script_register_inout("STRTOL",    NULL, th_strtol,    SCRIPT_T_INT);   /* 文字列→整数（既定16進）, v0.4 */
    script_register_inout("FIELD",     NULL, th_field,     SCRIPT_T_STR);   /* 区切りN番目トークン, v0.4 */
    script_register_inout("UPPER",     NULL, th_upper,     SCRIPT_T_STR);   /* 大文字化, v0.4 */
    script_register_inout("LOWER",     NULL, th_lower,     SCRIPT_T_STR);   /* 小文字化, v0.4 */
    script_register_inout("TRIMMER",   NULL, th_trimmer,   SCRIPT_T_STR);   /* 前後空白除去, v0.4 */
}
