/* yajir_libs.h - Flash 常駐のスクリプトライブラリ表（def_import の実機版, v0.4.10）
 *
 * def_import("名前") が来たときにコアへ渡す本文を、ここに const char[] で置く。
 * ライブラリの「所在」はホストの領分なので、PC版（ローカルファイル <名前>.yaj を読む）と
 * 実機版（この表を名前で引く）で決め方が違ってよい ── スクリプト側は同じ1行で書ける。
 *
 * 実機でこの方式が効く理由:
 *   - const char[] は **Flash に置かれる**。取り込みに RAM を1バイトも使わない。
 *   - コンパイラはこのポインタを直接読み、原文を保持しない（append-only 生成＋文字列はプールへ
 *     intern 済み）。だからライブラリ用の受信バッファも要らない。
 *
 * ライブラリを増やすときは (1) 本文の const char[] を足す (2) YAJIR_LIBS[] に1行足す、だけ。
 * ※本ヘッダは yajir_glue.c だけが include する前提（static 定義を持つ）。
 */
#ifndef YAJIR_LIBS_H
#define YAJIR_LIBS_H

/* scripts/lib/blinker.yaj と同じ内容（コメントは削って Flash を節約してある）。
 * 使用スロット: GVAR[6] / GVAR[7]。エクスポート: BLINK_MS / BLINK_ON / BLINK_START / BLINK_STOP。
 * アプリが ON BLINK_EVENT を書いていれば毎回呼ばれる（INVOKER＝未登録名は無視＝weak link）。 */
static const char YAJIR_LIB_BLINKER[] =
    "def_alias(BLINK_MS, GVAR[7])\n"
    "def_alias(BLINK_ON, GVAR[6])\n"
    "def_handler(BLINK_START)\n"
    "def_handler(BLINK_STOP)\n"
    "def_handler(BLINK_TICK)\n"
    "ON BLINK_START\n"
    "    (BLINK_MS == 0) -> IFYES\n"
    "        200 -> BLINK_MS\n"
    "    END\n"
    "    1 -> BLINK_ON\n"
    "    none -> BLINK_TICK AFTER BLINK_MS\n"
    "END\n"
    "ON BLINK_STOP\n"
    "    0 -> BLINK_ON\n"
    "END\n"
    "ON BLINK_TICK\n"
    "    (BLINK_ON == 0) -> IFYES\n"
    "        EXIT\n"
    "    END\n"
    "    NOT LED1 -> LED1\n"
    "    \"BLINK_EVENT\" -> INVOKER\n"
    "    none -> BLINK_TICK AFTER BLINK_MS\n"
    "END\n";

typedef struct {
    const char *name;   /* def_import("...") に書く名前 */
    const char *src;    /* 本文（NUL終端・Flash常駐） */
} yajir_lib_t;

static const yajir_lib_t YAJIR_LIBS[] = {
    { "blinker", YAJIR_LIB_BLINKER },
};
#define YAJIR_NLIBS ((int)(sizeof(YAJIR_LIBS) / sizeof(YAJIR_LIBS[0])))

#endif /* YAJIR_LIBS_H */
