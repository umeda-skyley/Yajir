/* netutil.c - PC専用ネットワークUtilityポート
 *
 * HTTP_SYNC: "https://example.com" -> HTTP_SYNC
 *   SRESULT = レスポンス本文（CFG_SSTR_LEN - 1 まで）
 *   RESULT  = HTTPステータスコード（取得できない/失敗時は0）
 *
 * HTTP_ASYNC: "https://example.com" -> HTTP_ASYNC
 *   完了時に HTTP イベントをpostする。
 *   ON HTTP では SARG[0]=本文, ARG[1]=ステータスコード。
 *
 * GPT_SYNC: "prompt" -> GPT_SYNC
 *   .env の OPENAI_API_KEY / OPENAI_MODEL を使い、Responses API を呼ぶ。
 *   SRESULT = 返答テキスト、RESULT = HTTPステータスコード。
 *
 * GPT_ASYNC: "prompt" -> GPT_ASYNC
 *   完了時に GPT イベントをpostする。
 *   ON GPT では SARG[0]=返答テキスト, ARG[1]=ステータスコード。
 *
 * CLAUDE_SYNC: "prompt" -> CLAUDE_SYNC
 *   .env の ANTHROPIC_API_KEY / CLAUDE_MODEL を使い、Messages API を呼ぶ。
 *   SRESULT = 返答テキスト、RESULT = HTTPステータスコード。
 *
 * CLAUDE_ASYNC: "prompt" -> CLAUDE_ASYNC
 *   完了時に CLAUDE イベントをpostする。
 *   ON CLAUDE では SARG[0]=返答テキスト, ARG[1]=ステータスコード。
 *
 * GEMINI_SYNC: "prompt" -> GEMINI_SYNC
 *   .env の GEMINI_API_KEY / GEMINI_MODEL を使い、Interactions API を呼ぶ。
 *   SRESULT = 返答テキスト、RESULT = HTTPステータスコード。
 *
 * GEMINI_ASYNC: "prompt" -> GEMINI_ASYNC
 *   完了時に GEMINI イベントをpostする。
 *   ON GEMINI では SARG[0]=返答テキスト, ARG[1]=ステータスコード。
 */
#include <windows.h>
#include <wininet.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include "script.h"
#include "vm.h"
#include "netutil.h"

#pragma comment(lib, "wininet.lib")

#define HTTP_TIMEOUT_MS 10000
#define GPT_TIMEOUT_MS 60000
#define GPT_DEFAULT_MODEL "gpt-5.5"
#define CLAUDE_TIMEOUT_MS 60000
#define CLAUDE_DEFAULT_MODEL "claude-sonnet-4-6"
#define CLAUDE_DEFAULT_MAX_TOKENS 1024
#define ANTHROPIC_VERSION "2023-06-01"
#define GEMINI_TIMEOUT_MS 60000
#define GEMINI_DEFAULT_MODEL "gemini-3.5-flash"
#define GEMINI_THINKING_LEVEL "medium"
#define GEMINI_RAW_RESPONSE_CAP (1024 * 1024)
#define AI_CHAT_HISTORY_CAP (256 * 1024)
#define GPT_ENV_VALUE_MAX 256

typedef struct {
    char *url;
} async_http_req_t;

typedef struct {
    char *prompt;
    char *model_override;
} GPT_req_t;

typedef struct {
    char *prompt;
    char *model_override;
} CLAUDE_req_t;

typedef struct {
    char *prompt;
    char *model_override;
} GEMINI_req_t;

typedef struct {
    char api_key[GPT_ENV_VALUE_MAX];
    char model[GPT_ENV_VALUE_MAX];
    char max_output_tokens[GPT_ENV_VALUE_MAX];
    char temperature[GPT_ENV_VALUE_MAX];
    char top_p[GPT_ENV_VALUE_MAX];
    char reasoning_effort[GPT_ENV_VALUE_MAX];
    char text_verbosity[GPT_ENV_VALUE_MAX];
    char instructions[GPT_ENV_VALUE_MAX];
} openai_env_t;

typedef struct {
    char api_key[GPT_ENV_VALUE_MAX];
    char model[GPT_ENV_VALUE_MAX];
    char max_tokens[GPT_ENV_VALUE_MAX];
    char temperature[GPT_ENV_VALUE_MAX];
    char top_p[GPT_ENV_VALUE_MAX];
    char top_k[GPT_ENV_VALUE_MAX];
    char thinking_type[GPT_ENV_VALUE_MAX];
    char thinking_budget_tokens[GPT_ENV_VALUE_MAX];
    char thinking_display[GPT_ENV_VALUE_MAX];
    char system[GPT_ENV_VALUE_MAX];
} claude_env_t;

typedef struct {
    char api_key[GPT_ENV_VALUE_MAX];
    char model[GPT_ENV_VALUE_MAX];
    char max_output_tokens[GPT_ENV_VALUE_MAX];
    char temperature[GPT_ENV_VALUE_MAX];
    char top_p[GPT_ENV_VALUE_MAX];
    char top_k[GPT_ENV_VALUE_MAX];
    char thinking_level[GPT_ENV_VALUE_MAX];
    char system_instruction[GPT_ENV_VALUE_MAX];
} gemini_env_t;

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} strbuf_t;

typedef enum {
    AI_CHAT_GPT = 0,
    AI_CHAT_CLAUDE,
    AI_CHAT_GEMINI,
    AI_CHAT_COUNT
} ai_chat_kind_t;

typedef struct {
    char *s;
    size_t len;
    size_t cap;
} ai_chat_history_t;

typedef int (*ai_request_fn_t)(const char *prompt, const char *model_override,
                               char *out, int cap, int32_t *status);

static ai_chat_history_t g_ai_chat[AI_CHAT_COUNT];
static CRITICAL_SECTION g_ai_chat_lock;
static int g_ai_chat_lock_ready = 0;

static int starts_json_object(const char *s);

static int sb_init(strbuf_t *b, size_t cap)
{
    b->s = (char *)malloc(cap);
    if (!b->s) { b->len = b->cap = 0; return 0; }
    b->len = 0;
    b->cap = cap;
    b->s[0] = '\0';
    return 1;
}

static int sb_reserve(strbuf_t *b, size_t add)
{
    size_t need = b->len + add + 1;
    char *p;
    if (need <= b->cap) return 1;
    while (b->cap < need) b->cap *= 2;
    p = (char *)realloc(b->s, b->cap);
    if (!p) return 0;
    b->s = p;
    return 1;
}

static int sb_append(strbuf_t *b, const char *s)
{
    size_t n = strlen(s ? s : "");
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->s + b->len, s ? s : "", n + 1);
    b->len += n;
    return 1;
}

static int sb_appendf(strbuf_t *b, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;
    int n;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !sb_reserve(b, (size_t)n)) { va_end(ap2); return 0; }
    vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 1;
}

static char *sb_take(strbuf_t *b)
{
    char *s = b->s;
    b->s = NULL;
    b->len = b->cap = 0;
    return s;
}

static void ai_chat_init(void)
{
    if (!g_ai_chat_lock_ready) {
        InitializeCriticalSection(&g_ai_chat_lock);
        g_ai_chat_lock_ready = 1;
    }
}

static void ai_chat_lock(void)
{
    if (!g_ai_chat_lock_ready) ai_chat_init();
    EnterCriticalSection(&g_ai_chat_lock);
}

static void ai_chat_unlock(void)
{
    LeaveCriticalSection(&g_ai_chat_lock);
}

static int ai_chat_reserve(ai_chat_history_t *h, size_t add)
{
    size_t need = h->len + add + 1;
    char *p;
    if (need <= h->cap) return 1;
    if (h->cap == 0) h->cap = 4096;
    while (h->cap < need) h->cap *= 2;
    p = (char *)realloc(h->s, h->cap);
    if (!p) return 0;
    h->s = p;
    if (h->len == 0) h->s[0] = '\0';
    return 1;
}

static int ai_chat_append_raw(ai_chat_history_t *h, const char *s)
{
    size_t n = strlen(s ? s : "");
    if (!ai_chat_reserve(h, n)) return 0;
    memcpy(h->s + h->len, s ? s : "", n + 1);
    h->len += n;
    return 1;
}

static void ai_chat_trim(ai_chat_history_t *h)
{
    size_t drop;
    char *nl;
    if (!h->s || h->len <= AI_CHAT_HISTORY_CAP) return;
    drop = h->len - AI_CHAT_HISTORY_CAP;
    nl = (char *)memchr(h->s + drop, '\n', h->len - drop);
    if (nl) drop = (size_t)(nl + 1 - h->s);
    memmove(h->s, h->s + drop, h->len - drop + 1);
    h->len -= drop;
}

static char *ai_chat_build_prompt(ai_chat_kind_t kind, const char *user)
{
    ai_chat_history_t *h;
    char *out = NULL;
    size_t user_len;
    size_t need;

    if (!user || !user[0]) return NULL;
    user_len = strlen(user);
    ai_chat_lock();
    h = &g_ai_chat[kind];
    need = h->len + user_len + 48;
    out = (char *)malloc(need);
    if (out) {
        if (h->len > 0) {
            snprintf(out, need, "%s\nUser:\n%s\nAssistant:\n", h->s, user);
        } else {
            snprintf(out, need, "User:\n%s\nAssistant:\n", user);
        }
    }
    ai_chat_unlock();
    return out;
}

static void ai_chat_commit(ai_chat_kind_t kind, const char *user, const char *assistant)
{
    ai_chat_history_t *h;
    if (!user || !user[0] || !assistant || !assistant[0]) return;
    ai_chat_lock();
    h = &g_ai_chat[kind];
    if (h->len > 0) ai_chat_append_raw(h, "\n");
    ai_chat_append_raw(h, "User:\n");
    ai_chat_append_raw(h, user);
    ai_chat_append_raw(h, "\nAssistant:\n");
    ai_chat_append_raw(h, assistant);
    ai_chat_append_raw(h, "\n");
    ai_chat_trim(h);
    ai_chat_unlock();
}

static int run_chat_request(ai_chat_kind_t kind, ai_request_fn_t fn,
                            const char *prompt, const char *model_override,
                            char *out, int cap, int32_t *status)
{
    char *chat_prompt = NULL;
    const char *send_prompt = prompt;
    int total_len;
    int use_chat = prompt && !starts_json_object(prompt);

    if (use_chat) {
        chat_prompt = ai_chat_build_prompt(kind, prompt);
        if (chat_prompt) send_prompt = chat_prompt;
    }

    total_len = fn(send_prompt, model_override, out, cap, status);
    if (use_chat && status && *status >= 200 && *status < 300 && out && out[0]) {
        ai_chat_commit(kind, prompt, out);
    }
    free(chat_prompt);
    return total_len;
}

static int32_t ai_chat_history_bytes(ai_chat_kind_t kind)
{
    size_t len;
    ai_chat_lock();
    len = g_ai_chat[kind].len;
    ai_chat_unlock();
    return len > INT32_MAX ? INT32_MAX : (int32_t)len;
}

static int32_t ai_chat_clear_if_zero(ai_chat_kind_t kind, int32_t value)
{
    ai_chat_history_t *h;
    size_t len;
    ai_chat_lock();
    h = &g_ai_chat[kind];
    if (value == 0) {
        h->len = 0;
        if (h->s) h->s[0] = '\0';
    }
    len = h->len;
    ai_chat_unlock();
    return len > INT32_MAX ? INT32_MAX : (int32_t)len;
}

static void set_internet_timeout(HINTERNET h, DWORD ms)
{
    InternetSetOptionA(h, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
    InternetSetOptionA(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    InternetSetOptionA(h, INTERNET_OPTION_SEND_TIMEOUT,    &ms, sizeof(ms));
}

static DWORD query_status_code(HINTERNET h)
{
    DWORD status = 0;
    DWORD len = sizeof(status);
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &len, NULL)) {
        return status;
    }
    return 0;
}

static int http_get(const char *url, char *out, int cap, int32_t *out_status)
{
    HINTERNET inet = NULL;
    HINTERNET req = NULL;
    DWORD flags;
    int out_len = 0;
    int total_len = 0;

    if (out_status) *out_status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!url || !url[0] || !out || cap <= 0) return 0;

    inet = InternetOpenA("Yajir/0.4 HTTP", INTERNET_OPEN_TYPE_PRECONFIG,
                         NULL, NULL, 0);
    if (!inet) return 0;
    set_internet_timeout(inet, HTTP_TIMEOUT_MS);

    flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
            INTERNET_FLAG_KEEP_CONNECTION;
    req = InternetOpenUrlA(inet, url, NULL, 0, flags, 0);
    if (!req) {
        InternetCloseHandle(inet);
        return 0;
    }
    set_internet_timeout(req, HTTP_TIMEOUT_MS);

    if (out_status) *out_status = (int32_t)query_status_code(req);

    for (;;) {
        char chunk[1024];
        DWORD got = 0;
        DWORD i;
        if (!InternetReadFile(req, chunk, sizeof(chunk), &got) || got == 0) break;
        total_len += (int)got;
        for (i = 0; i < got; i++) {
            if (out_len < cap - 1) out[out_len++] = chunk[i];
        }
    }
    out[out_len] = '\0';

    InternetCloseHandle(req);
    InternetCloseHandle(inet);
    return total_len;
}

static char *trim_ws(char *s)
{
    char *e;
    while (*s && isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void strip_quotes(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int env_set_value(char *dst, int cap, const char *value)
{
    char tmp[GPT_ENV_VALUE_MAX];
    strncpy(tmp, value ? value : "", sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strip_quotes(tmp);
    if (!tmp[0]) return 0;
    strncpy(dst, tmp, cap - 1);
    dst[cap - 1] = '\0';
    return 1;
}

static int load_openai_env(openai_env_t *env)
{
    char path[MAX_PATH];
    FILE *f;
    char line[512];

    memset(env, 0, sizeof(*env));
    strcpy(env->model, GPT_DEFAULT_MODEL);

    if (!GetCurrentDirectoryA(sizeof(path), path)) return 0;
    strncat(path, "\\.env", sizeof(path) - strlen(path) - 1);
    f = fopen(path, "rb");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim_ws(line);
        char *eq;
        char *name;
        char *value;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        name = trim_ws(p);
        value = trim_ws(eq + 1);
        if (strcmp(name, "OPENAI_API_KEY") == 0) env_set_value(env->api_key, sizeof(env->api_key), value);
        else if (strcmp(name, "OPENAI_MODEL") == 0) env_set_value(env->model, sizeof(env->model), value);
        else if (strcmp(name, "OPENAI_MAX_OUTPUT_TOKENS") == 0) env_set_value(env->max_output_tokens, sizeof(env->max_output_tokens), value);
        else if (strcmp(name, "OPENAI_TEMPERATURE") == 0) env_set_value(env->temperature, sizeof(env->temperature), value);
        else if (strcmp(name, "OPENAI_TOP_P") == 0) env_set_value(env->top_p, sizeof(env->top_p), value);
        else if (strcmp(name, "OPENAI_REASONING_EFFORT") == 0) env_set_value(env->reasoning_effort, sizeof(env->reasoning_effort), value);
        else if (strcmp(name, "OPENAI_TEXT_VERBOSITY") == 0) env_set_value(env->text_verbosity, sizeof(env->text_verbosity), value);
        else if (strcmp(name, "OPENAI_INSTRUCTIONS") == 0) env_set_value(env->instructions, sizeof(env->instructions), value);
    }
    fclose(f);
    return env->api_key[0] != '\0';
}

static int load_claude_env(claude_env_t *env)
{
    char path[MAX_PATH];
    FILE *f;
    char line[512];

    memset(env, 0, sizeof(*env));
    strcpy(env->model, CLAUDE_DEFAULT_MODEL);

    if (!GetCurrentDirectoryA(sizeof(path), path)) return 0;
    strncat(path, "\\.env", sizeof(path) - strlen(path) - 1);
    f = fopen(path, "rb");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim_ws(line);
        char *eq;
        char *name;
        char *value;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        name = trim_ws(p);
        value = trim_ws(eq + 1);
        if (strcmp(name, "ANTHROPIC_API_KEY") == 0) env_set_value(env->api_key, sizeof(env->api_key), value);
        else if (strcmp(name, "CLAUDE_MODEL") == 0) env_set_value(env->model, sizeof(env->model), value);
        else if (strcmp(name, "CLAUDE_MAX_TOKENS") == 0) env_set_value(env->max_tokens, sizeof(env->max_tokens), value);
        else if (strcmp(name, "CLAUDE_TEMPERATURE") == 0) env_set_value(env->temperature, sizeof(env->temperature), value);
        else if (strcmp(name, "CLAUDE_TOP_P") == 0) env_set_value(env->top_p, sizeof(env->top_p), value);
        else if (strcmp(name, "CLAUDE_TOP_K") == 0) env_set_value(env->top_k, sizeof(env->top_k), value);
        else if (strcmp(name, "CLAUDE_THINKING_TYPE") == 0) env_set_value(env->thinking_type, sizeof(env->thinking_type), value);
        else if (strcmp(name, "CLAUDE_THINKING_BUDGET_TOKENS") == 0) env_set_value(env->thinking_budget_tokens, sizeof(env->thinking_budget_tokens), value);
        else if (strcmp(name, "CLAUDE_THINKING_DISPLAY") == 0) env_set_value(env->thinking_display, sizeof(env->thinking_display), value);
        else if (strcmp(name, "CLAUDE_SYSTEM") == 0) env_set_value(env->system, sizeof(env->system), value);
    }
    fclose(f);
    return env->api_key[0] != '\0';
}

static int load_gemini_env(gemini_env_t *env)
{
    char path[MAX_PATH];
    FILE *f;
    char line[512];

    memset(env, 0, sizeof(*env));
    strcpy(env->model, GEMINI_DEFAULT_MODEL);

    if (!GetCurrentDirectoryA(sizeof(path), path)) return 0;
    strncat(path, "\\.env", sizeof(path) - strlen(path) - 1);
    f = fopen(path, "rb");
    if (!f) return 0;

    while (fgets(line, sizeof(line), f)) {
        char *p = trim_ws(line);
        char *eq;
        char *name;
        char *value;
        if (!*p || *p == '#') continue;
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        name = trim_ws(p);
        value = trim_ws(eq + 1);
        if (strcmp(name, "GEMINI_API_KEY") == 0 || strcmp(name, "GOOGLE_API_KEY") == 0) {
            env_set_value(env->api_key, sizeof(env->api_key), value);
        } else if (strcmp(name, "GEMINI_MODEL") == 0) {
            env_set_value(env->model, sizeof(env->model), value);
        } else if (strcmp(name, "GEMINI_MAX_OUTPUT_TOKENS") == 0) {
            env_set_value(env->max_output_tokens, sizeof(env->max_output_tokens), value);
        } else if (strcmp(name, "GEMINI_TEMPERATURE") == 0) {
            env_set_value(env->temperature, sizeof(env->temperature), value);
        } else if (strcmp(name, "GEMINI_TOP_P") == 0) {
            env_set_value(env->top_p, sizeof(env->top_p), value);
        } else if (strcmp(name, "GEMINI_TOP_K") == 0) {
            env_set_value(env->top_k, sizeof(env->top_k), value);
        } else if (strcmp(name, "GEMINI_THINKING_LEVEL") == 0) {
            env_set_value(env->thinking_level, sizeof(env->thinking_level), value);
        } else if (strcmp(name, "GEMINI_SYSTEM_INSTRUCTION") == 0) {
            env_set_value(env->system_instruction, sizeof(env->system_instruction), value);
        }
    }
    fclose(f);
    return env->api_key[0] != '\0';
}

static char *json_escape_alloc(const char *s)
{
    size_t need = 1;
    const unsigned char *p;
    char *out;
    char *q;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') need += 2;
        else if (*p == '\n' || *p == '\r' || *p == '\t') need += 2;
        else if (*p < 0x20) need += 6;
        else need++;
    }
    out = (char *)malloc(need);
    if (!out) return NULL;
    q = out;
    for (p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  *q++ = '\\'; *q++ = '"'; break;
        case '\\': *q++ = '\\'; *q++ = '\\'; break;
        case '\n': *q++ = '\\'; *q++ = 'n'; break;
        case '\r': *q++ = '\\'; *q++ = 'r'; break;
        case '\t': *q++ = '\\'; *q++ = 't'; break;
        default:
            if (*p < 0x20) {
                sprintf(q, "\\u%04x", (unsigned int)*p);
                q += 6;
            } else {
                *q++ = (char)*p;
            }
            break;
        }
    }
    *q = '\0';
    return out;
}

static int starts_json_object(const char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    return *s == '{';
}

static int json_has_key(const char *s, const char *key)
{
    char needle[64];
    if (!s || !key || strlen(key) + 2 >= sizeof(needle)) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    return strstr(s, needle) != NULL;
}

static int json_has_model(const char *s)
{
    return json_has_key(s, "model");
}

static int append_json_str_field(strbuf_t *b, int *need_comma, const char *key, const char *value)
{
    char *ev;
    int ok;
    if (!value || !value[0]) return 1;
    ev = json_escape_alloc(value);
    if (!ev) return 0;
    ok = sb_appendf(b, "%s\"%s\":\"%s\"", *need_comma ? "," : "", key, ev);
    *need_comma = 1;
    free(ev);
    return ok;
}

static int append_json_raw_field(strbuf_t *b, int *need_comma, const char *key, const char *value)
{
    if (!value || !value[0]) return 1;
    if (!sb_appendf(b, "%s\"%s\":%s", *need_comma ? "," : "", key, value)) return 0;
    *need_comma = 1;
    return 1;
}

static int append_json_obj_field(strbuf_t *b, int *need_comma, const char *key, const char *json_value)
{
    if (!json_value || !json_value[0]) return 1;
    if (!sb_appendf(b, "%s\"%s\":%s", *need_comma ? "," : "", key, json_value)) return 0;
    *need_comma = 1;
    return 1;
}

static char *build_json_with_model(const char *json, const char *model)
{
    const char *p = json;
    char *em = json_escape_alloc(model);
    char *out;
    size_t need;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!em || *p != '{') { free(em); return NULL; }
    need = strlen(json) + strlen(em) + 16;
    out = (char *)malloc(need);
    if (!out) { free(em); return NULL; }
    sprintf(out, "{\"model\":\"%s\",%s", em, p + 1);
    free(em);
    return out;
}

static char *build_gpt_body(const char *input, const char *model)
{
    char *ei = json_escape_alloc(input);
    char *em = json_escape_alloc(model);
    char *body;
    size_t need;
    if (!ei || !em) { free(ei); free(em); return NULL; }
    need = strlen(ei) + strlen(em) + 32;
    body = (char *)malloc(need);
    if (body) sprintf(body, "{\"model\":\"%s\",\"input\":\"%s\"}", em, ei);
    free(ei);
    free(em);
    return body;
}

static int append_openai_env_fields(strbuf_t *b, int *need_comma, const openai_env_t *env,
                                    const char *model, const char *src_json)
{
    char tmp[384];
    if (!src_json || !json_has_key(src_json, "model")) {
        if (!append_json_str_field(b, need_comma, "model", model)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "instructions")) {
        if (!append_json_str_field(b, need_comma, "instructions", env->instructions)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "max_output_tokens")) {
        if (!append_json_raw_field(b, need_comma, "max_output_tokens", env->max_output_tokens)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "temperature")) {
        if (!append_json_raw_field(b, need_comma, "temperature", env->temperature)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "top_p")) {
        if (!append_json_raw_field(b, need_comma, "top_p", env->top_p)) return 0;
    }
    if (env->reasoning_effort[0] && (!src_json || !json_has_key(src_json, "reasoning"))) {
        snprintf(tmp, sizeof(tmp), "{\"effort\":\"%s\"}", env->reasoning_effort);
        if (!append_json_obj_field(b, need_comma, "reasoning", tmp)) return 0;
    }
    if (env->text_verbosity[0] && (!src_json || !json_has_key(src_json, "text"))) {
        snprintf(tmp, sizeof(tmp), "{\"verbosity\":\"%s\"}", env->text_verbosity);
        if (!append_json_obj_field(b, need_comma, "text", tmp)) return 0;
    }
    return 1;
}

static char *build_gpt_body_env(const char *input, const openai_env_t *env, const char *model)
{
    strbuf_t b;
    char *ei = json_escape_alloc(input);
    int need_comma = 0;
    if (!ei || !sb_init(&b, strlen(input) + 256)) { free(ei); return NULL; }
    if (!sb_append(&b, "{")) goto fail;
    if (!append_openai_env_fields(&b, &need_comma, env, model, NULL)) goto fail;
    if (!sb_appendf(&b, "%s\"input\":\"%s\"", need_comma ? "," : "", ei)) goto fail;
    if (!sb_append(&b, "}")) goto fail;
    free(ei);
    return sb_take(&b);
fail:
    free(ei);
    free(b.s);
    return NULL;
}

static char *build_openai_json_with_defaults(const char *json, const openai_env_t *env, const char *model)
{
    const char *p = json;
    strbuf_t b;
    int need_comma = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' || !sb_init(&b, strlen(json) + 512)) return NULL;
    if (!sb_append(&b, "{")) goto fail;
    if (!append_openai_env_fields(&b, &need_comma, env, model, json)) goto fail;
    if (need_comma && !sb_append(&b, ",")) goto fail;
    if (!sb_append(&b, p + 1)) goto fail;
    return sb_take(&b);
fail:
    free(b.s);
    return NULL;
}

static char *make_gpt_request_body(const char *arg0, const openai_env_t *env, const char *model)
{
    if (starts_json_object(arg0)) {
        return build_openai_json_with_defaults(arg0, env, model);
    }
    return build_gpt_body_env(arg0, env, model);
}

static const char *claude_max_tokens_value(const claude_env_t *env)
{
    return env->max_tokens[0] ? env->max_tokens : "1024";
}

static int append_claude_env_fields(strbuf_t *b, int *need_comma, const claude_env_t *env,
                                    const char *model, const char *src_json)
{
    strbuf_t t;
    const char *thinking_type = env->thinking_type[0] ? env->thinking_type :
        (env->thinking_budget_tokens[0] ? "enabled" : "");

    if (!src_json || !json_has_key(src_json, "model")) {
        if (!append_json_str_field(b, need_comma, "model", model)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "max_tokens")) {
        if (!append_json_raw_field(b, need_comma, "max_tokens", claude_max_tokens_value(env))) return 0;
    }
    if (!src_json || !json_has_key(src_json, "temperature")) {
        if (!append_json_raw_field(b, need_comma, "temperature", env->temperature)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "top_p")) {
        if (!append_json_raw_field(b, need_comma, "top_p", env->top_p)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "top_k")) {
        if (!append_json_raw_field(b, need_comma, "top_k", env->top_k)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "system")) {
        if (!append_json_str_field(b, need_comma, "system", env->system)) return 0;
    }
    if (thinking_type[0] && (!src_json || !json_has_key(src_json, "thinking"))) {
        int tc = 0;
        if (!sb_init(&t, 128)) return 0;
        if (!sb_append(&t, "{")) goto fail;
        if (!append_json_str_field(&t, &tc, "type", thinking_type)) goto fail;
        if (strcmp(thinking_type, "disabled") != 0) {
            if (!append_json_raw_field(&t, &tc, "budget_tokens", env->thinking_budget_tokens)) goto fail;
            if (!append_json_str_field(&t, &tc, "display", env->thinking_display)) goto fail;
        }
        if (!sb_append(&t, "}")) goto fail;
        if (!append_json_obj_field(b, need_comma, "thinking", t.s)) goto fail;
        free(t.s);
    }
    return 1;
fail:
    free(t.s);
    return 0;
}

static char *build_claude_body(const char *input, const claude_env_t *env, const char *model)
{
    strbuf_t b;
    char *ei = json_escape_alloc(input);
    int need_comma = 0;
    if (!ei || !sb_init(&b, strlen(input) + 256)) { free(ei); return NULL; }
    if (!sb_append(&b, "{")) goto fail;
    if (!append_claude_env_fields(&b, &need_comma, env, model, NULL)) goto fail;
    if (!sb_appendf(&b, "%s\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]",
                    need_comma ? "," : "", ei)) goto fail;
    if (!sb_append(&b, "}")) goto fail;
    free(ei);
    return sb_take(&b);
fail:
    free(ei);
    free(b.s);
    return NULL;
}

static char *build_claude_json_with_defaults(const char *json, const claude_env_t *env, const char *model)
{
    const char *p = json;
    strbuf_t b;
    int need_comma = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' || !sb_init(&b, strlen(json) + 512)) return NULL;
    if (!sb_append(&b, "{")) goto fail;
    if (!append_claude_env_fields(&b, &need_comma, env, model, json)) goto fail;
    if (need_comma && !sb_append(&b, ",")) goto fail;
    if (!sb_append(&b, p + 1)) goto fail;
    return sb_take(&b);
fail:
    free(b.s);
    return NULL;
}

static char *make_claude_request_body(const char *arg0, const claude_env_t *env, const char *model)
{
    if (starts_json_object(arg0)) return build_claude_json_with_defaults(arg0, env, model);
    return build_claude_body(arg0, env, model);
}

static const char *gemini_thinking_level_value(const gemini_env_t *env)
{
    return env->thinking_level[0] ? env->thinking_level : GEMINI_THINKING_LEVEL;
}

static int append_gemini_generation_config(strbuf_t *b, int *need_comma, const gemini_env_t *env)
{
    strbuf_t g;
    int gc = 0;
    if (!sb_init(&g, 160)) return 0;
    if (!sb_append(&g, "{")) goto fail;
    if (!append_json_str_field(&g, &gc, "thinking_level", gemini_thinking_level_value(env))) goto fail;
    if (!append_json_raw_field(&g, &gc, "temperature", env->temperature)) goto fail;
    if (!append_json_raw_field(&g, &gc, "top_p", env->top_p)) goto fail;
    if (!append_json_raw_field(&g, &gc, "top_k", env->top_k)) goto fail;
    if (!append_json_raw_field(&g, &gc, "max_output_tokens", env->max_output_tokens)) goto fail;
    if (!sb_append(&g, "}")) goto fail;
    if (!append_json_obj_field(b, need_comma, "generation_config", g.s)) goto fail;
    free(g.s);
    return 1;
fail:
    free(g.s);
    return 0;
}

static int append_gemini_env_fields(strbuf_t *b, int *need_comma, const gemini_env_t *env,
                                    const char *model, const char *src_json)
{
    if (!src_json || !json_has_key(src_json, "model")) {
        if (!append_json_str_field(b, need_comma, "model", model)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "system_instruction")) {
        if (!append_json_str_field(b, need_comma, "system_instruction", env->system_instruction)) return 0;
    }
    if (!src_json || !json_has_key(src_json, "generation_config")) {
        if (!append_gemini_generation_config(b, need_comma, env)) return 0;
    }
    return 1;
}

static char *build_gemini_body(const char *input, const gemini_env_t *env, const char *model)
{
    strbuf_t b;
    char *ei = json_escape_alloc(input);
    int need_comma = 0;
    if (!ei || !sb_init(&b, strlen(input) + 256)) { free(ei); return NULL; }
    if (!sb_append(&b, "{")) goto fail;
    if (!append_gemini_env_fields(&b, &need_comma, env, model, NULL)) goto fail;
    if (!sb_appendf(&b, "%s\"input\":\"%s\"", need_comma ? "," : "", ei)) goto fail;
    if (!sb_append(&b, "}")) goto fail;
    free(ei);
    return sb_take(&b);
fail:
    free(ei);
    free(b.s);
    return NULL;
}

static char *build_gemini_json_with_defaults(const char *json, const gemini_env_t *env, const char *model)
{
    const char *p = json;
    strbuf_t b;
    int need_comma = 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '{' || !sb_init(&b, strlen(json) + 512)) return NULL;
    if (!sb_append(&b, "{")) goto fail;
    if (!append_gemini_env_fields(&b, &need_comma, env, model, json)) goto fail;
    if (need_comma && !sb_append(&b, ",")) goto fail;
    if (!sb_append(&b, p + 1)) goto fail;
    return sb_take(&b);
fail:
    free(b.s);
    return NULL;
}

static char *make_gemini_request_body(const char *arg0, const gemini_env_t *env, const char *model)
{
    if (starts_json_object(arg0)) return build_gemini_json_with_defaults(arg0, env, model);
    return build_gemini_body(arg0, env, model);
}

static int http_post_openai_responses(const char *api_key, const char *body, char *out, int cap, int32_t *out_status)
{
    HINTERNET inet = NULL;
    HINTERNET conn = NULL;
    HINTERNET req = NULL;
    char headers[512];
    int out_len = 0;
    int total_len = 0;
    BOOL ok;
    const char *accept[] = { "application/json", NULL };

    if (out_status) *out_status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!api_key || !api_key[0] || !body || !out || cap <= 0) return 0;

    inet = InternetOpenA("Yajir/0.4 GPT", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return 0;
    set_internet_timeout(inet, GPT_TIMEOUT_MS);

    conn = InternetConnectA(inet, "api.openai.com", INTERNET_DEFAULT_HTTPS_PORT,
                            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return 0; }

    req = HttpOpenRequestA(conn, "POST", "/v1/responses", NULL, NULL, accept,
                           INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                           INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!req) {
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        return 0;
    }
    set_internet_timeout(req, GPT_TIMEOUT_MS);

    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", api_key);
    ok = HttpSendRequestA(req, headers, (DWORD)-1, (LPVOID)body, (DWORD)strlen(body));
    if (ok) {
        if (out_status) *out_status = (int32_t)query_status_code(req);
        for (;;) {
            char chunk[1024];
            DWORD got = 0;
            DWORD i;
            if (!InternetReadFile(req, chunk, sizeof(chunk), &got) || got == 0) break;
            total_len += (int)got;
            for (i = 0; i < got; i++) {
                if (out_len < cap - 1) out[out_len++] = chunk[i];
            }
        }
        out[out_len] = '\0';
    }

    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return total_len;
}

static int http_post_anthropic_messages(const char *api_key, const char *body, char *out, int cap, int32_t *out_status)
{
    HINTERNET inet = NULL;
    HINTERNET conn = NULL;
    HINTERNET req = NULL;
    char headers[640];
    int out_len = 0;
    int total_len = 0;
    BOOL ok;
    const char *accept[] = { "application/json", NULL };

    if (out_status) *out_status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!api_key || !api_key[0] || !body || !out || cap <= 0) return 0;

    inet = InternetOpenA("Yajir/0.4 CLAUDE", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return 0;
    set_internet_timeout(inet, CLAUDE_TIMEOUT_MS);

    conn = InternetConnectA(inet, "api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT,
                            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return 0; }

    req = HttpOpenRequestA(conn, "POST", "/v1/messages", NULL, NULL, accept,
                           INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                           INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!req) {
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        return 0;
    }
    set_internet_timeout(req, CLAUDE_TIMEOUT_MS);

    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\n"
             "x-api-key: %s\r\n"
             "anthropic-version: %s\r\n",
             api_key, ANTHROPIC_VERSION);
    ok = HttpSendRequestA(req, headers, (DWORD)-1, (LPVOID)body, (DWORD)strlen(body));
    if (ok) {
        if (out_status) *out_status = (int32_t)query_status_code(req);
        for (;;) {
            char chunk[1024];
            DWORD got = 0;
            DWORD i;
            if (!InternetReadFile(req, chunk, sizeof(chunk), &got) || got == 0) break;
            total_len += (int)got;
            for (i = 0; i < got; i++) {
                if (out_len < cap - 1) out[out_len++] = chunk[i];
            }
        }
        out[out_len] = '\0';
    }

    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return total_len;
}

static int http_post_gemini_interactions(const char *api_key, const char *body, char *out, int cap, int32_t *out_status)
{
    HINTERNET inet = NULL;
    HINTERNET conn = NULL;
    HINTERNET req = NULL;
    char headers[512];
    int out_len = 0;
    int total_len = 0;
    BOOL ok;
    const char *accept[] = { "application/json", NULL };

    if (out_status) *out_status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!api_key || !api_key[0] || !body || !out || cap <= 0) return 0;

    inet = InternetOpenA("Yajir/0.4 GEMINI", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!inet) return 0;
    set_internet_timeout(inet, GEMINI_TIMEOUT_MS);

    conn = InternetConnectA(inet, "generativelanguage.googleapis.com", INTERNET_DEFAULT_HTTPS_PORT,
                            NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!conn) { InternetCloseHandle(inet); return 0; }

    req = HttpOpenRequestA(conn, "POST", "/v1beta/interactions", NULL, NULL, accept,
                           INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
                           INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!req) {
        InternetCloseHandle(conn);
        InternetCloseHandle(inet);
        return 0;
    }
    set_internet_timeout(req, GEMINI_TIMEOUT_MS);

    snprintf(headers, sizeof(headers),
             "Content-Type: application/json\r\n"
             "x-goog-api-key: %s\r\n",
             api_key);
    ok = HttpSendRequestA(req, headers, (DWORD)-1, (LPVOID)body, (DWORD)strlen(body));
    if (ok) {
        if (out_status) *out_status = (int32_t)query_status_code(req);
        for (;;) {
            char chunk[1024];
            DWORD got = 0;
            DWORD i;
            if (!InternetReadFile(req, chunk, sizeof(chunk), &got) || got == 0) break;
            total_len += (int)got;
            for (i = 0; i < got; i++) {
                if (out_len < cap - 1) out[out_len++] = chunk[i];
            }
        }
        out[out_len] = '\0';
    }

    InternetCloseHandle(req);
    InternetCloseHandle(conn);
    InternetCloseHandle(inet);
    return total_len;
}

static const char *skip_json_string(const char *p)
{
    if (*p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *find_json_key(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strchr(p, '"')) != NULL) {
        const char *end = skip_json_string(p);
        if (end > p + 1 && (size_t)(end - p - 2) == klen && strncmp(p + 1, key, klen) == 0) {
            const char *q = end;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q == ':') return q + 1;
        }
        p = end;
    }
    return NULL;
}

static int append_char(char *out, int cap, int *len, char c)
{
    if (*len >= cap - 1) { vm_set_err(ERR_STR_TRUNC); return 0; }
    out[(*len)++] = c;
    out[*len] = '\0';
    return 1;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *copy_json_string(const char *p, char *out, int cap, int *len)
{
    if (*p != '"') return p;
    p++;
    while (*p) {
        if (*p == '"') return p + 1;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': append_char(out, cap, len, '"'); p++; break;
            case '\\': append_char(out, cap, len, '\\'); p++; break;
            case '/': append_char(out, cap, len, '/'); p++; break;
            case 'b': append_char(out, cap, len, '\b'); p++; break;
            case 'f': append_char(out, cap, len, '\f'); p++; break;
            case 'n': append_char(out, cap, len, '\n'); p++; break;
            case 'r': append_char(out, cap, len, '\r'); p++; break;
            case 't': append_char(out, cap, len, '\t'); p++; break;
            case 'u': {
                int h0, h1, h2, h3, cp;
                if (!p[1] || !p[2] || !p[3] || !p[4]) return p;
                h0 = hexval(p[1]); h1 = hexval(p[2]); h2 = hexval(p[3]); h3 = hexval(p[4]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return p;
                cp = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
                if (cp < 0x80) {
                    append_char(out, cap, len, (char)cp);
                } else if (cp < 0x800) {
                    append_char(out, cap, len, (char)(0xC0 | (cp >> 6)));
                    append_char(out, cap, len, (char)(0x80 | (cp & 0x3F)));
                } else {
                    append_char(out, cap, len, (char)(0xE0 | (cp >> 12)));
                    append_char(out, cap, len, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    append_char(out, cap, len, (char)(0x80 | (cp & 0x3F)));
                }
                p += 5;
                break;
            }
            default:
                if (*p) { append_char(out, cap, len, *p); p++; }
                break;
            }
        } else {
            append_char(out, cap, len, *p++);
        }
    }
    return p;
}

static int copy_json_string_closed(const char *p, char *out, int cap, int *len)
{
    const char *end = copy_json_string(p, out, cap, len);
    return end > p && end[-1] == '"';
}

static int extract_first_string_key(const char *json, const char *key, char *out, int cap)
{
    const char *p = find_json_key(json, key);
    int len = 0;
    out[0] = '\0';
    if (!p) return 0;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    if (!copy_json_string_closed(p, out, cap, &len)) return 0;
    return len > 0;
}

static int json_key_eq(const char *start, const char *end, const char *key)
{
    size_t klen = strlen(key);
    return end > start + 1 &&
           (size_t)(end - start - 2) == klen &&
           strncmp(start + 1, key, klen) == 0;
}

static const char *json_value_after_colon(const char *p)
{
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return NULL;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int json_string_eq(const char *p, const char *value)
{
    const char *end;
    size_t vlen;
    if (!p || *p != '"') return 0;
    end = skip_json_string(p);
    vlen = strlen(value);
    return end > p + 1 &&
           (size_t)(end - p - 2) == vlen &&
           strncmp(p + 1, value, vlen) == 0;
}

static const char *find_enclosing_object_start(const char *json, const char *pos)
{
    const char *stack[64];
    int sp = 0;
    const char *p = json;
    while (*p && p < pos) {
        if (*p == '"') {
            p = skip_json_string(p);
            continue;
        }
        if (*p == '{') {
            if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) stack[sp++] = p;
            p++;
            continue;
        }
        if (*p == '}') {
            if (sp > 0) sp--;
            p++;
            continue;
        }
        p++;
    }
    return sp > 0 ? stack[sp - 1] : NULL;
}

static const char *find_matching_object_end(const char *start)
{
    const char *p = start;
    int depth = 0;
    if (!p || *p != '{') return NULL;
    while (*p) {
        if (*p == '"') {
            p = skip_json_string(p);
            continue;
        }
        if (*p == '{') {
            depth++;
            p++;
            continue;
        }
        if (*p == '}') {
            depth--;
            p++;
            if (depth == 0) return p;
            continue;
        }
        p++;
    }
    return NULL;
}

static int append_text_keys_in_range(const char *start, const char *end, char *out, int cap, int *len)
{
    const char *p = start;
    while (*p && p < end) {
        if (*p == '"') {
            const char *key_start = p;
            const char *key_end = skip_json_string(p);
            const char *value = json_value_after_colon(key_end);
            if (value && value < end && json_key_eq(key_start, key_end, "text") && *value == '"') {
                if (*len > 0) append_char(out, cap, len, '\n');
                if (!copy_json_string_closed(value, out, cap, len)) return 0;
            }
            p = key_end;
            continue;
        }
        p++;
    }
    return *len > 0;
}

static int extract_gpt_text(const char *json, char *out, int cap)
{
    const char *p = json;
    int len = 0;
    out[0] = '\0';
    while ((p = strstr(p, "\"type\"")) != NULL) {
        const char *colon = strchr(p, ':');
        if (!colon) break;
        colon++;
        while (*colon && isspace((unsigned char)*colon)) colon++;
        if (strncmp(colon, "\"output_text\"", 13) == 0) {
            const char *text = find_json_key(colon, "text");
            if (text) {
                while (*text && isspace((unsigned char)*text)) text++;
                if (*text == '"') {
                    if (len > 0) append_char(out, cap, &len, '\n');
                    if (!copy_json_string_closed(text, out, cap, &len)) return 0;
                }
            }
        }
        p = colon;
    }
    if (len > 0) return 1;
    return extract_first_string_key(json, "message", out, cap);
}

static int extract_claude_text(const char *json, char *out, int cap)
{
    const char *p = json;
    int len = 0;
    out[0] = '\0';
    while ((p = strstr(p, "\"type\"")) != NULL) {
        const char *colon = strchr(p, ':');
        if (!colon) break;
        colon++;
        while (*colon && isspace((unsigned char)*colon)) colon++;
        if (strncmp(colon, "\"text\"", 6) == 0) {
            const char *text = find_json_key(colon, "text");
            if (text) {
                while (*text && isspace((unsigned char)*text)) text++;
                if (*text == '"') {
                    if (len > 0) append_char(out, cap, &len, '\n');
                    if (!copy_json_string_closed(text, out, cap, &len)) return 0;
                }
            }
        }
        p = colon;
    }
    if (len > 0) return 1;
    if (extract_first_string_key(json, "text", out, cap)) return 1;
    return extract_first_string_key(json, "message", out, cap);
}

static int extract_gemini_text(const char *json, char *out, int cap)
{
    const char *p = json;
    int len = 0;

    if (extract_first_string_key(json, "output_text", out, cap)) return 1;

    out[0] = '\0';
    while ((p = strstr(p, "\"model_output\"")) != NULL) {
        const char *start = find_enclosing_object_start(json, p);
        const char *end = find_matching_object_end(start);
        if (start && end) {
            if (!append_text_keys_in_range(start, end, out, cap, &len)) return 0;
            p = end;
        } else {
            p += 14;
        }
    }
    if (len > 0) return 1;
    if (extract_first_string_key(json, "text", out, cap)) return 1;
    return extract_first_string_key(json, "message", out, cap);
}

static int run_gpt_request(const char *prompt, const char *model_override,
                           char *out, int cap, int32_t *status)
{
    const char *model;
    openai_env_t env;
    char *body = NULL;
    char *raw = NULL;
    int raw_cap = cap * 4;
    int total_len = 0;

    if (status) *status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!prompt || !prompt[0] || !out || cap <= 0) return 0;
    if (raw_cap < CFG_SSTR_LEN) raw_cap = CFG_SSTR_LEN;

    if (!load_openai_env(&env)) return 0;

    model = env.model;
    if (model_override && model_override[0]) model = model_override;

    body = make_gpt_request_body(prompt, &env, model);
    raw = (char *)malloc((size_t)raw_cap);
    if (!body || !raw) goto done;

    total_len = http_post_openai_responses(env.api_key, body, raw, raw_cap, status);
    if (total_len >= raw_cap) vm_set_err(ERR_STR_TRUNC);

    if (raw[0]) {
        if (!extract_gpt_text(raw, out, cap)) {
            int copy = (int)strlen(raw);
            if (copy >= cap) { copy = cap - 1; vm_set_err(ERR_STR_TRUNC); }
            memcpy(out, raw, copy);
            out[copy] = '\0';
        }
    }

done:
    free(raw);
    free(body);
    return total_len;
}

static int run_claude_request(const char *prompt, const char *model_override,
                              char *out, int cap, int32_t *status)
{
    const char *model;
    claude_env_t env;
    char *body = NULL;
    char *raw = NULL;
    int raw_cap = cap * 4;
    int total_len = 0;

    if (status) *status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!prompt || !prompt[0] || !out || cap <= 0) return 0;
    if (raw_cap < CFG_SSTR_LEN) raw_cap = CFG_SSTR_LEN;

    if (!load_claude_env(&env)) return 0;

    model = env.model;
    if (model_override && model_override[0]) model = model_override;

    body = make_claude_request_body(prompt, &env, model);
    raw = (char *)malloc((size_t)raw_cap);
    if (!body || !raw) goto done;

    total_len = http_post_anthropic_messages(env.api_key, body, raw, raw_cap, status);
    if (total_len >= raw_cap) vm_set_err(ERR_STR_TRUNC);

    if (raw[0]) {
        if (!extract_claude_text(raw, out, cap)) {
            int copy = (int)strlen(raw);
            if (copy >= cap) { copy = cap - 1; vm_set_err(ERR_STR_TRUNC); }
            memcpy(out, raw, copy);
            out[copy] = '\0';
        }
    }

done:
    free(raw);
    free(body);
    return total_len;
}

static int run_gemini_request(const char *prompt, const char *model_override,
                              char *out, int cap, int32_t *status)
{
    const char *model;
    gemini_env_t env;
    char *body = NULL;
    char *raw = NULL;
    int raw_cap = GEMINI_RAW_RESPONSE_CAP;
    int total_len = 0;

    if (status) *status = 0;
    if (out && cap > 0) out[0] = '\0';
    if (!prompt || !prompt[0] || !out || cap <= 0) return 0;
    if (raw_cap < cap * 4) raw_cap = cap * 4;

    if (!load_gemini_env(&env)) return 0;

    model = env.model;
    if (model_override && model_override[0]) model = model_override;

    body = make_gemini_request_body(prompt, &env, model);
    raw = (char *)malloc((size_t)raw_cap);
    if (!body || !raw) goto done;

    total_len = http_post_gemini_interactions(env.api_key, body, raw, raw_cap, status);
    if (total_len >= raw_cap) vm_set_err(ERR_STR_TRUNC);

    if (raw[0]) {
        if (!extract_gemini_text(raw, out, cap)) {
            int copy = (int)strlen(raw);
            if (copy >= cap) { copy = cap - 1; vm_set_err(ERR_STR_TRUNC); }
            memcpy(out, raw, copy);
            out[copy] = '\0';
        }
    }

done:
    free(raw);
    free(body);
    return total_len;
}

static void th_gpt(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    char out[CFG_SSTR_LEN];
    int32_t status = 0;

    out[0] = '\0';
    script_set_result(0);
    script_set_sresult("");

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    run_chat_request(AI_CHAT_GPT, run_gpt_request, prompt, model_override, out, sizeof(out), &status);
    script_set_result(status);
    script_set_sresult(out);
}

static void th_gemini(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    char out[CFG_SSTR_LEN];
    int32_t status = 0;

    out[0] = '\0';
    script_set_result(0);
    script_set_sresult("");

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    run_chat_request(AI_CHAT_GEMINI, run_gemini_request, prompt, model_override, out, sizeof(out), &status);
    script_set_result(status);
    script_set_sresult(out);
}

static void th_claude(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    char out[CFG_SSTR_LEN];
    int32_t status = 0;

    out[0] = '\0';
    script_set_result(0);
    script_set_sresult("");

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    run_chat_request(AI_CHAT_CLAUDE, run_claude_request, prompt, model_override, out, sizeof(out), &status);
    script_set_result(status);
    script_set_sresult(out);
}

static int32_t in_gpt_history(void)
{
    return ai_chat_history_bytes(AI_CHAT_GPT);
}

static void th_gpt_history(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    script_set_result(ai_chat_clear_if_zero(AI_CHAT_GPT, v));
}

static int32_t in_claude_history(void)
{
    return ai_chat_history_bytes(AI_CHAT_CLAUDE);
}

static void th_claude_history(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    script_set_result(ai_chat_clear_if_zero(AI_CHAT_CLAUDE, v));
}

static int32_t in_gemini_history(void)
{
    return ai_chat_history_bytes(AI_CHAT_GEMINI);
}

static void th_gemini_history(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    script_set_result(ai_chat_clear_if_zero(AI_CHAT_GEMINI, v));
}

static void th_http(int argc, const script_value_t *a)
{
    const char *url;
    int32_t status = 0;
    int total_len;
    char out[CFG_SSTR_LEN];

    out[0] = '\0';
    script_set_result(0);

    if (argc < 1 || !script_val_is_str(a[0])) {
        script_set_sresult("");
        return;
    }
    url = script_resolve_str(a[0]);
    if (!url || !url[0]) {
        script_set_sresult("");
        return;
    }

    total_len = http_get(url, out, CFG_SSTR_LEN, &status);
    if (total_len >= CFG_SSTR_LEN) vm_set_err(ERR_STR_TRUNC);
    script_set_result(status);
    script_set_sresult(out);
}

static DWORD WINAPI async_http_thread(LPVOID p)
{
    async_http_req_t *req = (async_http_req_t *)p;
    char *body;
    int32_t status = 0;
    int total_len = 0;
    script_arg_t args[2];

    body = (char *)malloc(CFG_SARG_LEN);
    if (body) total_len = http_get(req->url, body, CFG_SARG_LEN, &status);

    args[0] = SCRIPT_ARG_STR(body ? body : "", body ? total_len : 0);
    args[1] = SCRIPT_ARG_INT(status);
    script_post_msg_v("HTTP", 2, args);

    free(body);
    free(req->url);
    free(req);
    return 0;
}

static void th_http_async(int argc, const script_value_t *a)
{
    const char *url;
    async_http_req_t *req;
    HANDLE th;

    if (argc < 1 || !script_val_is_str(a[0])) return;
    url = script_resolve_str(a[0]);
    if (!url || !url[0]) return;

    req = (async_http_req_t *)malloc(sizeof(*req));
    if (!req) return;
    req->url = (char *)malloc(strlen(url) + 1);
    if (!req->url) { free(req); return; }
    strcpy(req->url, url);

    th = CreateThread(NULL, 0, async_http_thread, req, 0, NULL);
    if (th) CloseHandle(th);
    else { free(req->url); free(req); }
}

static char *copy_cstr(const char *s)
{
    char *p;
    size_t n = strlen(s ? s : "");
    p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s ? s : "", n + 1);
    return p;
}

static void post_GPT_complete(const char *text, int text_len, int32_t status)
{
    script_arg_t args[2];

    args[0] = SCRIPT_ARG_STR(text ? text : "", text ? text_len : 0);
    args[1] = SCRIPT_ARG_INT(status);
    script_post_msg_v("GPT", 2, args);
}

static DWORD WINAPI GPT_thread(LPVOID p)
{
    GPT_req_t *req = (GPT_req_t *)p;
    char *text;
    int32_t status = 0;

    text = (char *)malloc(CFG_SARG_LEN);
    if (text) {
        run_chat_request(AI_CHAT_GPT, run_gpt_request, req->prompt, req->model_override, text, CFG_SARG_LEN, &status);
    }

    post_GPT_complete(text ? text : "", text ? (int)strlen(text) : 0, status);

    free(text);
    free(req->prompt);
    free(req->model_override);
    free(req);
    return 0;
}

static void post_CLAUDE_complete(const char *text, int text_len, int32_t status)
{
    script_arg_t args[2];

    args[0] = SCRIPT_ARG_STR(text ? text : "", text ? text_len : 0);
    args[1] = SCRIPT_ARG_INT(status);
    script_post_msg_v("CLAUDE", 2, args);
}

static DWORD WINAPI CLAUDE_thread(LPVOID p)
{
    CLAUDE_req_t *req = (CLAUDE_req_t *)p;
    char *text;
    int32_t status = 0;

    text = (char *)malloc(CFG_SARG_LEN);
    if (text) {
        run_chat_request(AI_CHAT_CLAUDE, run_claude_request, req->prompt, req->model_override, text, CFG_SARG_LEN, &status);
    }

    post_CLAUDE_complete(text ? text : "", text ? (int)strlen(text) : 0, status);

    free(text);
    free(req->prompt);
    free(req->model_override);
    free(req);
    return 0;
}

static void post_GEMINI_complete(const char *text, int text_len, int32_t status)
{
    script_arg_t args[2];

    args[0] = SCRIPT_ARG_STR(text ? text : "", text ? text_len : 0);
    args[1] = SCRIPT_ARG_INT(status);
    script_post_msg_v("GEMINI", 2, args);
}

static DWORD WINAPI GEMINI_thread(LPVOID p)
{
    GEMINI_req_t *req = (GEMINI_req_t *)p;
    char *text;
    int32_t status = 0;

    text = (char *)malloc(CFG_SARG_LEN);
    if (text) {
        run_chat_request(AI_CHAT_GEMINI, run_gemini_request, req->prompt, req->model_override, text, CFG_SARG_LEN, &status);
    }

    post_GEMINI_complete(text ? text : "", text ? (int)strlen(text) : 0, status);

    free(text);
    free(req->prompt);
    free(req->model_override);
    free(req);
    return 0;
}

static void th_gpt_async(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    GPT_req_t *req;
    HANDLE th;

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    req = (GPT_req_t *)malloc(sizeof(*req));
    if (!req) return;
    req->prompt = copy_cstr(prompt);
    req->model_override = model_override && model_override[0] ? copy_cstr(model_override) : NULL;
    if (!req->prompt || (model_override && model_override[0] && !req->model_override)) {
        free(req->prompt);
        free(req->model_override);
        free(req);
        return;
    }

    th = CreateThread(NULL, 0, GPT_thread, req, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(req->prompt);
        free(req->model_override);
        free(req);
    }
}

static void th_gemini_async(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    GEMINI_req_t *req;
    HANDLE th;

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    req = (GEMINI_req_t *)malloc(sizeof(*req));
    if (!req) return;
    req->prompt = copy_cstr(prompt);
    req->model_override = model_override && model_override[0] ? copy_cstr(model_override) : NULL;
    if (!req->prompt || (model_override && model_override[0] && !req->model_override)) {
        free(req->prompt);
        free(req->model_override);
        free(req);
        return;
    }

    th = CreateThread(NULL, 0, GEMINI_thread, req, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(req->prompt);
        free(req->model_override);
        free(req);
    }
}

static void th_claude_async(int argc, const script_value_t *a)
{
    const char *prompt;
    const char *model_override = NULL;
    CLAUDE_req_t *req;
    HANDLE th;

    if (argc < 1 || !script_val_is_str(a[0])) return;
    prompt = script_resolve_str(a[0]);
    if (!prompt || !prompt[0]) return;
    if (argc >= 2 && script_val_is_str(a[1])) {
        model_override = script_resolve_str(a[1]);
    }

    req = (CLAUDE_req_t *)malloc(sizeof(*req));
    if (!req) return;
    req->prompt = copy_cstr(prompt);
    req->model_override = model_override && model_override[0] ? copy_cstr(model_override) : NULL;
    if (!req->prompt || (model_override && model_override[0] && !req->model_override)) {
        free(req->prompt);
        free(req->model_override);
        free(req);
        return;
    }

    th = CreateThread(NULL, 0, CLAUDE_thread, req, 0, NULL);
    if (th) CloseHandle(th);
    else {
        free(req->prompt);
        free(req->model_override);
        free(req);
    }
}

void register_netutils(void)
{
    ai_chat_init();
    script_register_inout("HTTP_SYNC", NULL, th_http, SCRIPT_T_STR);
    script_register_out("HTTP_ASYNC", th_http_async);
    script_register_handler("HTTP");
    script_register_inout("GPT_SYNC", NULL, th_gpt, SCRIPT_T_STR);
    script_register_out("GPT_ASYNC", th_gpt_async);
    script_register_handler("GPT");
    script_register_inout("GPT_HISTORY", in_gpt_history, th_gpt_history, SCRIPT_T_INT);
    script_register_inout("CLAUDE_SYNC", NULL, th_claude, SCRIPT_T_STR);
    script_register_out("CLAUDE_ASYNC", th_claude_async);
    script_register_handler("CLAUDE");
    script_register_inout("CLAUDE_HISTORY", in_claude_history, th_claude_history, SCRIPT_T_INT);
    script_register_inout("GEMINI_SYNC", NULL, th_gemini, SCRIPT_T_STR);
    script_register_out("GEMINI_ASYNC", th_gemini_async);
    script_register_handler("GEMINI");
    script_register_inout("GEMINI_HISTORY", in_gemini_history, th_gemini_history, SCRIPT_T_INT);
}
