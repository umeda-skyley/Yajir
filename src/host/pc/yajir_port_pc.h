/* yajir_port_pc.h - PC host hooks for core critical sections */
#ifndef YAJIR_PORT_PC_H
#define YAJIR_PORT_PC_H

void yj_pc_enter_critical(void);
void yj_pc_exit_critical(void);

#define YJ_ENTER_CRITICAL() yj_pc_enter_critical()
#define YJ_EXIT_CRITICAL()  yj_pc_exit_critical()

#endif /* YAJIR_PORT_PC_H */
