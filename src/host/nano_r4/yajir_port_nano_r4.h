/* yajir_port_nano_r4.h - interrupt-safe event queue hooks */
#ifndef YAJIR_PORT_NANO_R4_H
#define YAJIR_PORT_NANO_R4_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
uint32_t yajir_irq_save(void);
void yajir_irq_restore(uint32_t state);
#ifdef __cplusplus
}
#endif

#define YJ_ENTER_CRITICAL() uint32_t _yj_irq_state = yajir_irq_save()
#define YJ_EXIT_CRITICAL()  yajir_irq_restore(_yj_irq_state)

#endif /* YAJIR_PORT_NANO_R4_H */
