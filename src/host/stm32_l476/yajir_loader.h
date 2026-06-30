/* yajir_loader.h - UART流し込みローダ＋メインループ（STM32 Nucleo-L476RG）
 *
 * Tera Term 等でスクリプトを貼り付け、末尾に「@run」行で コンパイル＆実行する。
 * これは実機の `while(1){ if(block_ready) script_load(); script_tick(); }` の実体（§1）。
 *
 * 受信プロトコル（既存ST920ホストと同じ流儀）:
 *   <スクリプト本文…>
 *   @run     ← この行で「受信完了→ロード→実行開始」
 *   （@fin ならロードせず保存のみ。やり直しは MCU リセット）
 *
 * 統合（CubeMX 生成 main.c 側）:
 *   yajir_glue_init(&huart2);
 *   yajir_loader_init();
 *   HAL_UART_Receive_IT(&huart2, &rx, 1);     // 1バイト受信を起動
 *   while (1) { yajir_loop(); }
 *   // 割り込みコールバックから:
 *   void HAL_UART_RxCpltCallback(UART_HandleTypeDef *h){ yajir_feed_byte(rx); HAL_UART_Receive_IT(h,&rx,1); }
 *   void HAL_GPIO_EXTI_Callback(uint16_t pin){ if(pin==GPIO_PIN_13) yajir_post_button(); }
 */
#ifndef YAJIR_LOADER_H
#define YAJIR_LOADER_H

#include <stdint.h>

/* バナー表示＋受信バッファ初期化（電源ON/リセット後に1回）。 */
void yajir_loader_init(void);

/* UART受信1バイトを供給（RxCpltCallback から呼ぶ＝ISR文脈）。
 *   - 実行前: スクリプトバッファへ蓄積。末尾が "@run"/"@fin" になったらロード起動。
 *   - 実行後: その1バイトを UART1 イベントとして post（ON UART1 で受けられる）。 */
void yajir_feed_byte(uint8_t b);

/* ボタンEXTI から呼ぶ（ISR文脈）。次tickで ON BTN を発火。 */
void yajir_post_button(void);

/* メインループ本体。実行中なら script_tick() を1回回す。while(1)から毎周回呼ぶ。 */
void yajir_loop(void);

#endif /* YAJIR_LOADER_H */
