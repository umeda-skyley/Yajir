/* yajir_config_pico2_w.h - Pico 2 W sizing overrides */
#ifndef YAJIR_CONFIG_PICO2_W_H
#define YAJIR_CONFIG_PICO2_W_H

#define CFG_GVAR_COUNT          32
#define CFG_VAR_COUNT           32
#define CFG_ARG_COUNT            8
#define CFG_SGVAR_COUNT         16
#define CFG_SVAR_COUNT          16
#define CFG_SARG_COUNT           8
#define CFG_SSTR_LEN          1024
#define CFG_SARG_LEN          1024

#define CFG_MAX_PORTS          160
#define CFG_MAX_BLOCKS          48
#define CFG_MAX_RESOURCES       64
#define CFG_MAX_ALIAS           64
#define CFG_STACK_DEPTH         64
#define CFG_NEST_LIMIT           8
#define CFG_LOOP_NEST            8
#define CFG_CALL_NEST            8
#define CFG_MAX_SCRIPT_PORTS    32
#define CFG_INSTR_BUDGET     50000

/* Each queued event owns CFG_SARG_COUNT string buffers. Keeping this at 16
 * leaves room for the VM, CYW43 driver, USB stack, and application stacks. */
#define CFG_EVENT_QUEUE_LEN     16
#define CFG_TIMER_SLOTS         16
#define CFG_CODE_SIZE        16384
#define CFG_STRPOOL_SIZE     16384
#define CFG_MAX_NAME            32

#endif /* YAJIR_CONFIG_PICO2_W_H */
