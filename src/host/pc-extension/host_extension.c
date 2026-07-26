/* host_extension.c - 標準PCホストを拡張ポート付きで差し替える実装
 *
 * ここは「実機なら周辺ドライバ/ISR/CランタイムでつながるC資源」のPC代替。
 * コアはこの実装を一切知らず、register系API経由でのみ呼ぶ。
 * エラー文字列化と did-you-mean は common/host_diag.c に括り出した（全プラットフォーム共通）。
 *
 *   - get_tick()      : 単調増加ms（NOWへ束縛）
 *   - host_write_utf8 : コアの STDOUT が生成したUTF-8文字列をコンソールへ出力
 *   - LED1            : GPIO別名。状態変化をコンソールへ
 *   - net/file utils  : PC専用のHTTP/AI/FILEポート群
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "host_mock.h"
#include "script.h"
#include "host_diag.h"   /* host_diag_reset / host_diag_note（did-you-mean 候補収集） */
#include "netutil.h"
#include "fileutil.h"

/* PC hostのイベントenqueueを守る短いクリティカルセクション。
 * script_config.h の YJ_PORT_HEADER 経由で YJ_ENTER_CRITICAL/YJ_EXIT_CRITICAL から呼ばれる。 */
static CRITICAL_SECTION g_yj_lock;
static int g_yj_lock_ready = 0;

void yj_pc_enter_critical(void)
{
    if (g_yj_lock_ready) EnterCriticalSection(&g_yj_lock);
}

void yj_pc_exit_critical(void)
{
    if (g_yj_lock_ready) LeaveCriticalSection(&g_yj_lock);
}

/* register + 候補収集（host_diag_note）を一手にやる薄いラッパ。 */
static void reg_out  (const char *n, script_out_fn f){ script_register_out(n, f); host_diag_note(n); }
static void reg_inout(const char *n, script_in_fn g, script_out_fn s, script_type_t t){ script_register_inout(n, g, s, t); host_diag_note(n); }
static void reg_handler(const char *n){ script_register_handler(n); host_diag_note(n); }

/* ---- 単調クロック ---- */
static unsigned long long g_start = 0;
int32_t get_tick(void)
{
    unsigned long long t = GetTickCount64();
    if (g_start == 0) g_start = t;
    return (int32_t)(t - g_start);
}

/* ---- STDOUT（UTF-8対応の my_print_f 相当）---- */
static void host_write_utf8(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
        if (need > 0) {
            WCHAR stack_buf[512];
            WCHAR *w = stack_buf;
            DWORD written = 0;
            if (need > (int)(sizeof(stack_buf) / sizeof(stack_buf[0]))) {
                w = (WCHAR *)malloc((size_t)need * sizeof(WCHAR));
                if (!w) return;
            }
            MultiByteToWideChar(CP_UTF8, 0, s, -1, w, need);
            WriteConsoleW(h, w, (DWORD)(need - 1), &written, NULL);
            if (w != stack_buf) free(w);
            return;
        }
    }
    fputs(s, stdout);
}

/* ---- def_import: 標準PCホストと同じローカルファイル方式 ---- */
static char g_libbuf[4096];
static int pc_import(const char *name, const char **src, uint32_t *len)
{
    static const char *const dirs[] = { "", "scripts/lib/" };
    char path[256];
    size_t i, n;

    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        FILE *f;

        snprintf(path, sizeof(path), "%s%s.yaj", dirs[i], name);
        f = fopen(path, "rb");
        if (!f) continue;
        n = fread(g_libbuf, 1, sizeof(g_libbuf), f);
        if (n >= sizeof(g_libbuf)) {
            fclose(f);
            fprintf(stderr,
                    "[script] library '%s' is larger than the import buffer (%u bytes)\n",
                    name, (unsigned)sizeof(g_libbuf));
            return -1;
        }
        fclose(f);
        g_libbuf[n] = '\0';
        printf("[script] import '%s' <- %s (%u bytes)\n",
               name, path, (unsigned)n);
        *src = g_libbuf;
        *len = (uint32_t)n;
        return 0;
    }
    return -1;
}

static int32_t my_delay(int a) {
    uint32_t now = (uint32_t)get_tick();
    while( (uint32_t)(get_tick() - now) < (uint32_t)a );
    return 0;
}
static void th_delay(int argc, const script_value_t* a)
{
    int32_t x = (argc > 0) ? a[0].i : 0;
    my_delay(x);   /* DELAY は産出none の out（ブロッキング遅延・値を返さない, v0.3.8） */
}

/* ---- GPIO（LED1）：状態変化をコンソールへ ---- */
static int32_t g_led1 = 0;
static int32_t led1_get(void)   { return g_led1; }
/* inout(int): 書き込み＋「現在値を RESULT へ産出」（§3 tee, v0.3.8）。 */
static void led1_set(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    if (v != g_led1) { g_led1 = v; printf("[GPIO] LED1   -> %d\n", (int)v); }
    script_set_result(g_led1);   /* 現在値を産出（int → RESULT） */
}

/* ---- def_* 行に対応する束縛（マクロの実体）---- */
void host_register_all(void)
{
    g_start = GetTickCount64();
    if (!g_yj_lock_ready) { InitializeCriticalSection(&g_yj_lock); g_yj_lock_ready = 1; }
    host_diag_reset();   /* 再ロード時も did-you-mean 候補を作り直す（重複防止） */

    reg_inout("LED1",   led1_get,   led1_set,   SCRIPT_T_INT);
    script_register_now(get_tick);
    script_register_stdout(host_write_utf8);
    script_register_import(pc_import);
    reg_out  ("DELAY", th_delay);   /* ブロッキング遅延＝産出none の out */

    register_netutils();   /* HTTP_SYNC/HTTP_ASYNC, GPT/CLAUDE/GEMINI 系 */
    register_fileutils();  /* FILE_READER/FILE_WRITER */

    /* netutil.c 側で登録されるPC専用ポートを did-you-mean 候補にも載せる。 */
    host_diag_note("HTTP_SYNC");     /* URL -> HTTP_SYNC で本文を SRESULT、status を RESULT へ */
    host_diag_note("HTTP_ASYNC");    /* URL -> HTTP_ASYNC で非同期GET開始 */
    host_diag_note("HTTP");          /* 完了イベント: SARG[0]=本文, ARG[1]=status */
    host_diag_note("GPT_SYNC");      /* prompt/json -> GPT_SYNC でOpenAI Responses APIを呼ぶ */
    host_diag_note("GPT_ASYNC");     /* prompt/json -> GPT_ASYNC で非同期GPT開始 */
    host_diag_note("GPT");           /* 完了イベント: SARG[0]=返答, ARG[1]=status */
    host_diag_note("GPT_HISTORY");
    host_diag_note("CLAUDE_SYNC");   /* prompt/json -> CLAUDE_SYNC でAnthropic Messages APIを呼ぶ */
    host_diag_note("CLAUDE_ASYNC");  /* prompt/json -> CLAUDE_ASYNC で非同期Claude開始 */
    host_diag_note("CLAUDE");        /* 完了イベント: SARG[0]=返答, ARG[1]=status */
    host_diag_note("CLAUDE_HISTORY");
    host_diag_note("GEMINI_SYNC");   /* prompt/json -> GEMINI_SYNC でGoogle Gemini Interactions APIを呼ぶ */
    host_diag_note("GEMINI_ASYNC");  /* prompt/json -> GEMINI_ASYNC で非同期Gemini開始 */
    host_diag_note("GEMINI");        /* 完了イベント: SARG[0]=返答, ARG[1]=status */
    host_diag_note("GEMINI_HISTORY");

    reg_handler("UART1");
    reg_handler("BTN");
    reg_handler("MYHANDLER");       /* ユーザ定義の名前付きハンドラ（C/script 両方からpost可） */
}

/* C側から MYHANDLER を叩きたいとき用のサンプル（ISR/タスク等から呼ぶ）。 */
void host_fire_myhandler(int32_t a, int32_t b, int32_t c)
{
    script_arg_t args[3];
    args[0] = SCRIPT_ARG_INT(a);
    args[1] = SCRIPT_ARG_INT(b);
    args[2] = SCRIPT_ARG_INT(c);
    script_post_msg_v("MYHANDLER", 3, args);   /* 次tickで ON MYHANDLER が ARG[0..2] で受ける */
}
