/* yajir_port_pico2_w.h - Pico SDK critical-section hooks */
#ifndef YAJIR_PORT_PICO2_W_H
#define YAJIR_PORT_PICO2_W_H

#include <stdint.h>
#include "hardware/sync.h"

#define YJ_ENTER_CRITICAL() uint32_t _yj_irq_state = save_and_disable_interrupts()
#define YJ_EXIT_CRITICAL()  restore_interrupts(_yj_irq_state)

#endif /* YAJIR_PORT_PICO2_W_H */
