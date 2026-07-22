/* yajir_libs.h - Nano R4 Flash-resident script libraries */
#ifndef YAJIR_LIBS_NANO_R4_H
#define YAJIR_LIBS_NANO_R4_H

/* Uses GVAR[6] and GVAR[7]. Exports BLINK_MS, BLINK_ON,
 * BLINK_START, BLINK_STOP, and BLINK_EVENT through INVOKER. */
static const char YAJIR_LIB_BLINKER[] =
    "def_alias(BLINK_MS, GVAR[7])\n"
    "def_alias(BLINK_ON, GVAR[6])\n"
    "def_handler(BLINK_START)\n"
    "def_handler(BLINK_STOP)\n"
    "def_handler(BLINK_TICK)\n"
    "ON BLINK_START\n"
    "(BLINK_MS == 0) -> IFYES\n"
    "200 -> BLINK_MS\n"
    "END\n"
    "1 -> BLINK_ON\n"
    "none -> BLINK_TICK AFTER BLINK_MS\n"
    "END\n"
    "ON BLINK_STOP\n"
    "0 -> BLINK_ON\n"
    "END\n"
    "ON BLINK_TICK\n"
    "(BLINK_ON == 0) -> IFYES\n"
    "EXIT\n"
    "END\n"
    "NOT LED1 -> LED1\n"
    "\"BLINK_EVENT\" -> INVOKER\n"
    "none -> BLINK_TICK AFTER BLINK_MS\n"
    "END\n";

typedef struct {
    const char *name;
    const char *source;
} yajir_lib_t;

static const yajir_lib_t YAJIR_LIBS[] = {
    { "blinker", YAJIR_LIB_BLINKER },
};

#define YAJIR_NLIBS ((int)(sizeof(YAJIR_LIBS) / sizeof(YAJIR_LIBS[0])))

#endif /* YAJIR_LIBS_NANO_R4_H */
