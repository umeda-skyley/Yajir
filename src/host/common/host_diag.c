/* host_diag.c - host_diag.h の実装（PC / STM32 共通・純C）
 *
 * 元は PC ホスト(host_mock.c)に同居していたものを、プラットフォーム共通部として括り出した。
 * 中身はホスト依存のI/Oを一切持たない（文字列照合と編集距離だけ）。
 */
#include <string.h>
#include <ctype.h>
#include "host_diag.h"

/* ---- ロードエラーの文字列化（(B)方針：コアでなくホスト側で持つ, §11） ---- */
const char *script_strerror(script_err_t code)
{
    switch (code) {
        case ERR_NONE:           return "no error";
        case ERR_END_EXPECTED:   return "expected END (unclosed block)";
        case ERR_UNEXPECTED_END: return "unexpected END (no matching header)";
        case ERR_UNKNOWN_PORT:   return "unknown port";
        case ERR_UNKNOWN_NAME:   return "unknown name";
        case ERR_TYPE_MISMATCH:  return "type mismatch (int/string slot)";
        case ERR_WAIT_IN_ON:     return "WAIT not allowed in ON handler";
        case ERR_BAD_SLOT_INDEX: return "bad slot index (must be constant & in range)";
        case ERR_NEST_TOO_DEEP:  return "IFYES nesting too deep";
        case ERR_BAD_POSITION:   return "bad port position (in on right / out on left / wrong direction)";
        case ERR_SYNTAX:         return "syntax error";
    }
    return "load error";
}

/* ===== did-you-mean（ロードエラーの綴り間違い推定） =====
 *
 * コアは「未登録の tok」しか返さない。"近い名前"の知識＝何を register したかは、
 * ホストにしか無い。そこで:
 *   1) register 時に名前を控える（host_diag_note）＝唯一の情報源。
 *   2) これにコンパイラ予約の組込み名（スロット/Utility/STATUS/ERR_*）を足す。
 *   3) ERR_UNKNOWN_PORT / ERR_UNKNOWN_NAME のとき、編集距離が最も近い候補を1つ提案。 */

/* (1) ホストが実際に register したポート名を収集する箱 */
static const char *g_port_names[32];
static int         g_port_count = 0;

void host_diag_reset(void) { g_port_count = 0; }
void host_diag_note(const char *n)
{
    if (g_port_count < (int)(sizeof(g_port_names) / sizeof(g_port_names[0])))
        g_port_names[g_port_count++] = n;
}

/* (2) コンパイラ側で意味を持つ予約名（host は register しないが綴り間違いは起きる）。
 *     式中の打ち間違い（ERR_UNKNOWN_NAME）をここで拾えるようにする。 */
static const char *const g_builtin_names[] = {
    "GVAR","VAR","ARG","RESULT","SVAR","SGVAR","SARG","SRESULT","STATUS",
    "CODE_USED","STR_USED","VERSION",
    "SLICER","MERGER","COUNTER","FORMATTER","EQUALS","FINDER","STRTOL",
    "FIELD","UPPER","LOWER","TRIMMER","INVOKER",
    "ERR_QUEUE_OVF","ERR_TIMER_FULL","ERR_DIVZERO","ERR_STR_TRUNC",
    "WAIT","CLEAR_ERR","HANDLER",
    NULL
};

/* (3) 大小無視の Damerau-Levenshtein 距離（OSA版：隣接2文字の入れ替えを距離1で数える）。
 *     "LDE1"→"LED1" のような打ち間違い（隣接スワップ）を素のLevenshteinの2でなく1にする。 */
static int dl_ci(const char *a, const char *b)
{
    int la = (int)strlen(a), lb = (int)strlen(b), i, j;
    int d[CFG_MAX_NAME + 1][CFG_MAX_NAME + 1];
    if (la > CFG_MAX_NAME) la = CFG_MAX_NAME;
    if (lb > CFG_MAX_NAME) lb = CFG_MAX_NAME;
    for (i = 0; i <= la; i++) d[i][0] = i;
    for (j = 0; j <= lb; j++) d[0][j] = j;
    for (i = 1; i <= la; i++) {
        int ca = tolower((unsigned char)a[i - 1]);
        for (j = 1; j <= lb; j++) {
            int cb   = tolower((unsigned char)b[j - 1]);
            int cost = (ca == cb) ? 0 : 1;
            int del  = d[i - 1][j] + 1, ins = d[i][j - 1] + 1, sub = d[i - 1][j - 1] + cost;
            int m    = del < ins ? del : ins;
            m = m < sub ? m : sub;
            if (i > 1 && j > 1 &&
                ca == tolower((unsigned char)b[j - 2]) &&
                tolower((unsigned char)a[i - 2]) == cb) {
                int swp = d[i - 2][j - 2] + 1;
                if (swp < m) m = swp;
            }
            d[i][j] = m;
        }
    }
    return d[la][lb];
}

/* 最近傍の候補を返す。十分近くなければ NULL（的外れな提案を避ける）。 */
const char *host_suggest_name(const char *tok)
{
    const char *best = NULL;
    int bestd = 99, i, tlen = (int)strlen(tok), thresh;
    if (tlen == 0) return NULL;
    for (i = 0; i < g_port_count; i++) {
        int d = dl_ci(tok, g_port_names[i]);
        if (d < bestd) { bestd = d; best = g_port_names[i]; }
    }
    for (i = 0; g_builtin_names[i]; i++) {
        int d = dl_ci(tok, g_builtin_names[i]);
        if (d < bestd) { bestd = d; best = g_builtin_names[i]; }
    }
    /* 短い名前ほど厳しく：<=4字は距離1まで、それ以上は距離2まで提案 */
    thresh = (tlen <= 4) ? 1 : 2;
    if (best && bestd > 0 && bestd <= thresh) return best;
    return NULL;
}
