/* yajir_config_cardputer.h - ESP32-S3 internal-SRAM profile */
#ifndef YAJIR_CONFIG_CARDPUTER_H
#define YAJIR_CONFIG_CARDPUTER_H

#define CFG_GVAR_COUNT          16
#define CFG_VAR_COUNT           16
#define CFG_ARG_COUNT            8
#define CFG_SGVAR_COUNT          8
#define CFG_SVAR_COUNT           8
#define CFG_SARG_COUNT           4
#define CFG_SSTR_LEN           256
#define CFG_SARG_LEN           256

#define CFG_MAX_PORTS           96
#define CFG_MAX_BLOCKS          48
#define CFG_MAX_RESOURCES       32
#define CFG_MAX_ALIAS           64
#define CFG_MAX_IMPORTS         12
#define CFG_STACK_DEPTH         32
#define CFG_NEST_LIMIT           8
#define CFG_LOOP_NEST            8
#define CFG_CALL_NEST            4
#define CFG_MAX_SCRIPT_PORTS    32
#define CFG_INSTR_BUDGET     30000

#define CFG_EVENT_QUEUE_LEN      8
#define CFG_TIMER_SLOTS          8
#define CFG_DELAY_SLOTS          4
#define CFG_CODE_SIZE        16384
#define CFG_STRPOOL_SIZE      8192
#define CFG_MAX_NAME            32

#ifndef YAJIR_SOURCE_BUFFER_SIZE
#define YAJIR_SOURCE_BUFFER_SIZE 32768
#endif

#ifndef YAJIR_IMPORT_BUFFER_SIZE
#define YAJIR_IMPORT_BUFFER_SIZE 8192
#endif

#endif /* YAJIR_CONFIG_CARDPUTER_H */
