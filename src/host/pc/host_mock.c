/* host_mock.c - PC用ホスト依存部のモック実装（仕様 §11 ホスト依存部）
 *
 * ここは「実機なら周辺ドライバ/ISR/CランタイムでつながるC資源」のPC代替。
 * コアはこの実装を一切知らず、register系API経由でのみ呼ぶ。
 * エラー文字列化と did-you-mean は common/host_diag.c に括り出した（全プラットフォーム共通）。
 *
 *   - get_tick()      : 単調増加ms（NOW＝内部クロックの源。script_register_now で束ねる, v0.4.8）
 *   - pc_puts()       : STDOUT のシンク（出力先だけ）。ループ/タグ判定/int→10進/改行はコアが持つ
 *   - LED1            : GPIO別名。状態変化をコンソールへ
 *   - th_delay        : ブロッキング遅延（協調を無視する検証用の悪役）
 *
 * 登録する顔ぶれは stm32_l476/yajir_glue.c と揃えてある（差分はイベント源のみ）。
 * ここに増やすときは「実機側にも同じ名前が置けるか」を先に考えること。
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include "host_mock.h"
#include "script.h"
#include "host_diag.h"   /* host_diag_reset / host_diag_note（did-you-mean 候補収集） */

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

/* ---- STDOUT のシンク（出力先だけ）。整形はコアの th_stdout_core が担う（v0.4.8） ----
 * main.c が setvbuf(_IONBF) 済みなので fflush 不要（fputs だけでよい）。 */
static void pc_puts(const char *s) { fputs(s, stdout); }

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
static int32_t led1_get(void) { return g_led1; }
/* inout(int): 書き込み＋「現在値を RESULT へ産出」（§3 tee, v0.3.8）。 */
static void led1_set(int argc, const script_value_t *a)
{
    int32_t v = (argc > 0) ? a[0].i : 0;
    if (v != g_led1) { g_led1 = v; printf("[GPIO] LED1   -> %d\n", (int)v); }
    script_set_result(g_led1);   /* 現在値を産出（int → RESULT） */
}

/* ---- def_import: ライブラリの所在＝ホストの領分（§13, v0.4.10）----
 * PC版は「ローカルファイル <名前>.yaj」方式。def_import("blinker") → blinker.yaj を読む。
 * 実機（stm32_l476/yajir_libs.h）は Flash 常駐の const char[] 表を引く方式で、コアからは
 * どちらも同じ1本の関数に見える＝所在の決め方をホストごとに変えられる、というのが狙い。
 *
 * バッファは1本で足りる: 入れ子 import は禁止＝同時に開くライブラリは1つ、しかもコンパイラは
 * ソースを後戻りして読まないので、解析が終われば原文は不要（実機がバッファを持たずに済むのと同じ理屈）。 */
static char g_libbuf[4096];
static int pc_import(const char *name, const char **src, uint32_t *len)
{
    /* 探索順: カレント直下 → scripts/lib/（リポジトリ直下から実行したとき用） */
    static const char *const dirs[] = { "", "scripts/lib/" };
    char path[256];
    size_t i, n;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        FILE *f;
        snprintf(path, sizeof path, "%s%s.yaj", dirs[i], name);
        f = fopen(path, "rb");
        if (!f) continue;
        n = fread(g_libbuf, 1, sizeof(g_libbuf), f);
        if (n >= sizeof(g_libbuf)) {   /* 切り詰めた本文を渡すと嘘の構文エラーになる。無かった事にする */
            fclose(f);
            fprintf(stderr, "[script] library '%s' is larger than the import buffer (%u bytes)\n",
                    name, (unsigned)sizeof(g_libbuf));
            return -1;
        }
        fclose(f);
        g_libbuf[n] = '\0';
        printf("[script] import '%s' <- %s (%u bytes)\n", name, path, (unsigned)n);
        *src = g_libbuf;
        *len = (uint32_t)n;
        return 0;
    }
    return -1;   /* 見つからない → コアが ERR_IMPORT_NOT_FOUND でロード失敗にする */
}

/* ---- def_* 行に対応する束縛（マクロの実体）---- */
void host_register_all(void)
{
    g_start = GetTickCount64();
    host_diag_reset();   /* 再ロード時も did-you-mean 候補を作り直す（重複防止） */

    /* 実機(yajir_glue.c)と同じ顔ぶれ。片方だけに足さないこと（host/README.md）。 */
    reg_inout("LED1",   led1_get, led1_set, SCRIPT_T_INT);
    reg_out  ("DELAY",  th_delay);   /* ブロッキング遅延＝産出none の out */

    /* コア昇格した2つ（v0.4.8）。NOW=内部クロックの源、STDOUT=シンクだけ渡す。
     * did-you-mean 候補は host_diag の g_builtin_names 側で持つ（コア builtin 扱い）。 */
    script_register_now(get_tick);
    script_register_stdout(pc_puts);
    script_register_import(pc_import);   /* def_import の実体＝ローカルファイル（v0.4.10） */

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
