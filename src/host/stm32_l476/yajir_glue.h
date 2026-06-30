/* yajir_glue.h - STM32 Nucleo-L476RG 用ホストグルー（ボード依存部）
 *
 * これは「実機の周辺機能（GPIO/UART/SysTick）をコアのポートに束ねる」層。
 * CubeMX が生成した main.c から呼び出して使う（統合手順は README.md 参照）。
 *
 * ボード割り当て（Nucleo-L476RG 既定）:
 *   - LED1   : PA5  （ユーザLED LD2・緑）
 *   - BUTTON : PC13 （ユーザボタン B1・青, EXTI, アクティブLow）
 *   - UART   : USART2 115200 8N1（ST-LINK 仮想COMポート＝USB経由でTeraTermに繋がる）
 *
 * コアとは register系API でのみ接続し、コアはこのファイルの存在を知らない（§11）。
 */
#ifndef YAJIR_GLUE_H
#define YAJIR_GLUE_H

#include <stdint.h>
#include "stm32l4xx_hal.h"   /* CubeMX 生成の HAL（UART_HandleTypeDef 等） */

/* ボード初期化：STDOUT/ローダ出力に使う UART ハンドルを記憶する。
 * CubeMX の MX_USART2_UART_Init() 後に1回呼ぶ。 */
void yajir_glue_init(UART_HandleTypeDef *huart);

/* def_* 行に対応する register_* を一括実行（LED1/BUTTON/STDOUT/DELAY/NOW/UART1）。
 * script_init() の直後に呼ぶ（ローダがやるので通常は直接呼ばない）。 */
void host_register_all(void);

/* 単調増加ms（NOW ポートへ束ねる）。HAL_GetTick() の薄いラッパ。 */
int32_t get_tick(void);

/* UART へ1文字／文字列を送る（STDOUT とローダのメッセージが共用）。 */
void yajir_putc(char c);
void yajir_puts(const char *s);

#endif /* YAJIR_GLUE_H */
