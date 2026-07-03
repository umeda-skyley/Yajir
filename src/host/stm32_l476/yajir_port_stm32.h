/* yajir_port_stm32.h - Yajir enqueue クリティカルセクションの STM32(Cortex-M) 実装
 *
 * 生ISRからの直接post（MPSC安全, 仕様 §10/§11 v0.4.1）を有効にするため、コアの
 * YJ_ENTER_CRITICAL / YJ_EXIT_CRITICAL を CMSIS の PRIMASK 退避に割り当てる。
 * これでボタンEXTI/UART RX 等の割り込みハンドラから script_post_msg* を直接呼べる。
 *
 * 有効化の方法（CubeIDE）:
 *   Project > Properties > C/C++ Build > Settings > Tool Settings >
 *     MCU GCC Compiler > Preprocessor > Defined symbols に次を1つ追加:
 *       YJ_PORT_HEADER="yajir_port_stm32.h"
 *   さらに Include paths に本ファイルのディレクトリ（src/host/stm32_l476）を含める。
 *   → script_config.h が本ヘッダを取り込み、evq_push が割り込み禁止区間で保護される。
 *
 * 未設定なら YJ_*_CRITICAL は空（コア既定）＝ISR直postは非保護のままなので、実機ホスト
 * では設定推奨（このホストは UART RX ISR も直postするため、MPSC保護が要る）。
 */
#ifndef YAJIR_PORT_STM32_H
#define YAJIR_PORT_STM32_H

#include "stm32l4xx_hal.h"   /* __get_PRIMASK / __disable_irq / __set_PRIMASK (CMSIS) */

/* PRIMASK を退避してから割り込み禁止 → 復帰で元の状態へ戻す（既に禁止中でも壊さない＝再入安全）。
 * ENTER が保存変数を宣言し EXIT がそれを使うので、両者は同一ブロックで対に使うこと
 * （コア evq_push はこの規約で書かれている, §11）。 */
#define YJ_ENTER_CRITICAL()   uint32_t _yj_primask = __get_PRIMASK(); __disable_irq()
#define YJ_EXIT_CRITICAL()    __set_PRIMASK(_yj_primask)

#endif /* YAJIR_PORT_STM32_H */
