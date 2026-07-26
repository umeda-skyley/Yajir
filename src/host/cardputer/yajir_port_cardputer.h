/* yajir_port_cardputer.h - ESP32 critical-section hooks */
#ifndef YAJIR_PORT_CARDPUTER_H
#define YAJIR_PORT_CARDPUTER_H

#ifdef __cplusplus
extern "C" {
#endif

void yajir_critical_enter(void);
void yajir_critical_exit(void);

#ifdef __cplusplus
}
#endif

#define YJ_ENTER_CRITICAL() yajir_critical_enter()
#define YJ_EXIT_CRITICAL()  yajir_critical_exit()

#endif /* YAJIR_PORT_CARDPUTER_H */
