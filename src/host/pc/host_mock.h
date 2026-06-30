/* host_mock.h - PC用ホストグルー（薄い層）
 *
 * 実機ではこの層を board ごとに差し替える（src/host/stm32_l476/ 等）。
 * コア(script.h)とは register系API で接続する。共通の診断は common/host_diag.h。
 */
#ifndef HOST_MOCK_H
#define HOST_MOCK_H

#include <stdint.h>

/* 単調増加ms（gettimeofday相当, 実機はSysTick等）。NOWポートに束ねる */
int32_t get_tick(void);

/* §14 の def_* 行に対応する register_* を一括実行する（def_*マクロの実体）。 */
void host_register_all(void);

/* C側から名前付きハンドラ MYHANDLER を叩くサンプル poster（次tickで ON MYHANDLER）。 */
void host_fire_myhandler(int32_t a, int32_t b, int32_t c);

#endif /* HOST_MOCK_H */
