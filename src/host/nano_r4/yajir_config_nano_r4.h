/* yajir_config_nano_r4.h - small-memory Arduino Nano R4 profile */
#ifndef YAJIR_CONFIG_NANO_R4_H
#define YAJIR_CONFIG_NANO_R4_H

#define CFG_GVAR_COUNT           8
#define CFG_VAR_COUNT            8
#define CFG_ARG_COUNT            4
#define CFG_SGVAR_COUNT          4
#define CFG_SVAR_COUNT           4
#define CFG_SARG_COUNT           2
#define CFG_SSTR_LEN            32
#define CFG_SARG_LEN            32

/* The v0.4.9 core and mandatory NOW/STDOUT already occupy 34 ports. */
#define CFG_MAX_PORTS           64
#define CFG_MAX_BLOCKS           8
#define CFG_MAX_RESOURCES       12
#define CFG_MAX_ALIAS           16
#define CFG_MAX_IMPORTS          2
#define CFG_STACK_DEPTH         16
#define CFG_NEST_LIMIT           3
#define CFG_LOOP_NEST            2
#define CFG_CALL_NEST            2
#define CFG_MAX_SCRIPT_PORTS     8
#define CFG_INSTR_BUDGET     10000

#define CFG_EVENT_QUEUE_LEN      2
#define CFG_TIMER_SLOTS          4
#define CFG_DELAY_SLOTS          2
#define CFG_CODE_SIZE         1024
#define CFG_STRPOOL_SIZE       256

#ifndef YAJIR_SOURCE_BUFFER_SIZE
#define YAJIR_SOURCE_BUFFER_SIZE 4096
#endif

/* Includes the trailing NUL: identifiers are limited to 16 ASCII bytes. */
#define CFG_MAX_NAME            17

#endif /* YAJIR_CONFIG_NANO_R4_H */
