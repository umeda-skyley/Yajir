/* script.c - ホスト統合API実装＋ポート表管理（仕様 §11）
 *
 * register_* / set_result / init はフェーズ1から使う中核。
 * load（コンパイル差し替え）はフェーズ2,5、tick/post はフェーズ3,4 で実装する。
 *
 * ホスト非依存（純C）コア層。
 */
#include <string.h>
#include "script.h"
#include "vm.h"
#include "strutil.h"

/* ---- ポート表ヘルパ（コンパイラからも使う） ---- */

int vm_find_port(const char *name)
{
    script_vm_t *m = vm();
    int i;
    if (!m) return -1;
    for (i = 0; i < m->nports; i++)
        if (strcmp(m->ports[i].name, name) == 0) return i;
    return -1;
}

static port_t *add_port(const char *name, port_kind_t kind)
{
    script_vm_t *m = vm();
    port_t *pt;
    int existing = vm_find_port(name);
    /* HANDLER は組込みの予約ハンドラ源チャネル（register_builtins が立てる）。ホストが
     * 別種（out/in 等）で上書きしようとしても無視して保護する（v0.3.7+ 一本化）。 */
    if (existing >= 0 && kind != PK_HANDLER && strcmp(name, "HANDLER") == 0) return NULL;
    if (existing >= 0) {            /* 同名は上書き再束縛 */
        pt = &m->ports[existing];
    } else {
        if (m->nports >= CFG_MAX_PORTS) return NULL;
        pt = &m->ports[m->nports++];
    }
    memset(pt, 0, sizeof(*pt));
    strncpy(pt->name, name, CFG_MAX_NAME - 1);
    pt->name[CFG_MAX_NAME - 1] = '\0';
    pt->kind = kind;
    return pt;
}

/* ---- STATUS（§12） ----
 * status（divzero/strtrunc 等・tick文脈）に、ISRで立つキュー/タイマのboolを畳んで返す。
 * 実機ではキュー溢れフラグの読み書きをISRと共有するため atomic 化が必要（PC参照実装は単スレッド）。 */
static int32_t status_folded(void)
{
    script_vm_t *m = vm();
    int32_t s = m->status;
    if (m->evq_overflow)   s |= ERR_QUEUE_OVF;
    if (m->timer_overflow) s |= ERR_TIMER_FULL;
    return s;
}
static int32_t status_port_get(void) { return status_folded(); }  /* STATUS 入力ポート getter */

int32_t script_get_status(void) { return status_folded(); }

void script_clear_status(int32_t bits)
{
    script_vm_t *m = vm();
    m->status &= ~bits;
    if (bits & ERR_QUEUE_OVF)  m->evq_overflow = false;
    if (bits & ERR_TIMER_FULL) m->timer_overflow = false;
}

/* INVOKER（組み込みメタ・可変arity）: 第1引数の文字列名のハンドラ源へ、残り引数を
 * そのまま転送して post する動的ディスパッチ（"NAME", a, b, … -> INVOKER ＝ 次tickで ON NAME）。
 * 未登録/ハンドラ源でない名前は黙って無視（遅延束縛）。産出none の out（§10, v0.3.8）。
 * 名前は実行時に決まるので、コマンド名→ハンドラのジャンプテーブルや状態機械が書ける。 */
static void th_invoker(int argc, const script_value_t *a)
{
    extern int sched_enqueue_handler_vals(int handler_port, const value_t *pos, int n);
    const char *name;
    int pi;
    if (argc < 1 || !val_is_str(a[0])) return;                 /* 名前(文字列)が無ければ無視 */
    name = vm_resolve_str(a[0]);
    pi = vm_find_port(name);
    if (pi < 0 || vm()->ports[pi].kind != PK_HANDLER) return;  /* 未登録/非ハンドラ → 無視 */
    sched_enqueue_handler_vals(pi, &a[1], argc - 1);           /* 残り引数を転送（次tick・非再帰） */
}

/* ロード済みスクリプトの資源使用量＋バージョン（読み取り用の組込み入力ポート, v0.4）。 */
static int32_t code_used_get(void) { return (int32_t)vm()->code_len; }     /* バイトコード使用バイト数 */
static int32_t str_used_get (void) { return (int32_t)vm()->strpool_len; }  /* 文字列定数プール使用バイト数 */
static int32_t version_get  (void) { script_set_sresult(SCRIPT_VERSION); return 0; }  /* str産出→SRESULT（M3経路） */

const char *script_version(void) { return SCRIPT_VERSION; }

/* どのシステムでも最初から存在する組み込みポート/定数（§3, §12）。
 * ユーザーの register_* / def_* とは別枠で、script_init 時に登録する。 */
static void register_builtins(void)
{
    script_register_in("STATUS", status_port_get, SCRIPT_T_INT);   /* (STATUS & ERR_x) -> IFYES */
    script_register_in("CODE_USED", code_used_get, SCRIPT_T_INT);  /* バイトコード使用量（§12, v0.4） */
    script_register_in("STR_USED",  str_used_get,  SCRIPT_T_INT);  /* 文字列プール使用量（v0.4） */
    script_register_in("VERSION",   version_get,   SCRIPT_T_STR);  /* バージョン文字列（v0.4） */
    script_register_const("ERR_QUEUE_OVF",  ERR_QUEUE_OVF);
    script_register_const("ERR_TIMER_FULL", ERR_TIMER_FULL);
    script_register_const("ERR_DIVZERO",    ERR_DIVZERO);
    script_register_const("ERR_STR_TRUNC",  ERR_STR_TRUNC);
    register_strutils();   /* SLICER/MERGER/COUNTER/FORMATTER/EQUALS/FINDER（§3） */
    script_register_out("INVOKER", th_invoker);   /* 動的ディスパッチ（メタ・可変arity, §10） */
    /* HANDLER は「最初から在るハンドラ源チャネル1本」。これで -> HANDLER / ON HANDLER は
     * 名前付きハンドラ（MYHANDLER 等）と完全に同じ経路を通る（v0.3.7+ 一本化）。 */
    script_register_handler("HANDLER");
}

/* ---- 初期化（§11, §12） ---- */
void script_init(void *arena, size_t arena_size)
{
    vm_place_arena(arena, arena_size);
    register_builtins();   /* STATUS / ERR_* を常備（CLEAR_ERR はコンパイラ予約） */
}

/* ---- コンパイル時バインド（§11） ---- */
void script_register_const(const char *name, int32_t value)
{
    port_t *pt = add_port(name, PK_CONST);
    if (pt) pt->const_val = value;
}
void script_register_out(const char *name, script_out_fn fn)
{
    port_t *pt = add_port(name, PK_OUT);
    if (pt) pt->set_fn = fn;
}
void script_register_in(const char *name, script_in_fn fn, script_type_t out_type)
{
    port_t *pt = add_port(name, PK_IN);
    if (pt) { pt->get_fn = fn; pt->out_type = out_type; }
}
void script_register_inout(const char *name, script_in_fn get_fn, script_out_fn set_fn, script_type_t out_type)
{
    port_t *pt = add_port(name, PK_INOUT);
    if (pt) { pt->get_fn = get_fn; pt->set_fn = set_fn; pt->out_type = out_type; }
}
void script_register_handler(const char *name)
{
    add_port(name, PK_HANDLER);
}

/* ---- RESULT（§4） ---- */
void script_set_result(int32_t v)
{
    vm()->result = val_int(v);
}

const char *script_str(int32_t offset)
{
    return vm_str(offset);
}

int script_val_is_str(script_value_t v) { return val_is_str(v); }
const char *script_resolve_str(script_value_t v) { return vm_resolve_str(v); }
void script_set_sresult(const char *s) { vm_store_sstr(SSLOT_SRESULT, 0, s); }

/* ---- 観測用 ---- */
int script_event_overflow(void) { return vm() && vm()->evq_overflow ? 1 : 0; }
int script_timer_overflow(void) { return vm() && vm()->timer_overflow ? 1 : 0; }

/* ============================================================
 * 以下はフェーズ2以降で実装する。フェーズ1のVM単体テストでは未使用。
 * ============================================================ */

/* フェーズ2,5: トークナイズ→ワンパス・コンパイル→差し替え。
 * 戻り値 = 0=ok / 非0=script_err_t コード（v0.3.7）。詳細は script_last_error()。 */
int script_load(const char *src, size_t len)
{
    extern int compiler_compile(const char *src, size_t len);  /* compiler.c */
    return compiler_compile(src, len);
}

const script_error_t *script_last_error(void)
{
    extern const script_error_t *compiler_last_error(void);
    return compiler_last_error();
}

/* フェーズ4: イベントキューへ積む（ISR安全）。v0.3.5 で多値 post_msg_v 追加 */
int script_post_msg(const char *name, int32_t value)
{
    extern int sched_post(const char *name, value_t v);
    return sched_post(name, val_int(value));
}
int script_post_msg_char(const char *name, char ch)
{
    extern int sched_post(const char *name, value_t v);
    return sched_post(name, val_char((unsigned char)ch));
}
int script_post_msg_v(const char *name, int argc, const script_arg_t *argv)
{
    extern int sched_post_v(const char *name, const script_arg_t *args, int n);
    return sched_post_v(name, argv, argc);   /* 内部 sched_post_v は (name, args, n) のまま流用 */
}

/* フェーズ3,4: 1ステップ実行（タイマ満期＋キュー＋周期＋MAIN） */
void script_tick(void)
{
    extern void sched_tick(void);
    sched_tick();
}
