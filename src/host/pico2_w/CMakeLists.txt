cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w CACHE STRING "Pico board")
endif()

include(pico_sdk_import.cmake)

project(yajir_pico2_w C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(YAJIR_SRC_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_executable(yajir_pico2_w
    main.c
    yajir_glue.c
    yajir_loader.c
    ${YAJIR_SRC_ROOT}/core/compiler.c
    ${YAJIR_SRC_ROOT}/core/mathutil.c
    ${YAJIR_SRC_ROOT}/core/scheduler.c
    ${YAJIR_SRC_ROOT}/core/script.c
    ${YAJIR_SRC_ROOT}/core/strutil.c
    ${YAJIR_SRC_ROOT}/core/tokenizer.c
    ${YAJIR_SRC_ROOT}/core/vm.c
    ${YAJIR_SRC_ROOT}/host/common/host_diag.c
)

target_include_directories(yajir_pico2_w PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${YAJIR_SRC_ROOT}/core
    ${YAJIR_SRC_ROOT}/host/common
)

target_compile_definitions(yajir_pico2_w PRIVATE
    YJ_CONFIG_HEADER=\"yajir_config_pico2_w.h\"
    YJ_PORT_HEADER=\"yajir_port_pico2_w.h\"
)

target_link_libraries(yajir_pico2_w
    pico_stdlib
    pico_cyw43_arch_none
    hardware_adc
    hardware_clocks
    hardware_pwm
    hardware_sync
)

pico_enable_stdio_usb(yajir_pico2_w 1)
pico_enable_stdio_uart(yajir_pico2_w 0)
pico_add_extra_outputs(yajir_pico2_w)
