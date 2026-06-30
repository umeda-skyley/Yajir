/* scheduler.h - 協調スケジューラ＋イベント/タイマ（仕様 §1, §8, §10, §15 フェーズ3,4）
 * ホスト非依存（純C）コア層。 */
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "value.h"
#include "script.h"   /* script_arg_t */

/* 単値イベントをキューへ積む（post_msg/char 用）。0=ok / <0=満杯（§10） */
int  sched_post(const char *name, value_t v);

/* マルチ引数イベントをアトミックに積む（v0.3.5, §11）。0=ok / <0=満杯 or 未登録 */
int  sched_post_v(const char *name, const script_arg_t *args, int n);

/* スクリプト内 … -> <ハンドラ源>（HANDLER / MYHANDLER 等の名前付きハンドラ, v0.3.7+）。
 * EVT_HANDLER として該当 handler_port に積む。Cからの post_msg_v と同経路で ON <名前> が受ける。
 * 非再帰・次tick。組込み HANDLER もこの仕組みの上に乗る（特殊機構は廃止）。 */
int  sched_enqueue_handler_vals(int handler_port, const value_t *pos, int n);

/* 1 tick進める：タイマ満期→キュー→周期ON→MAIN（§1, §11） */
void sched_tick(void);

#endif /* SCHEDULER_H */
