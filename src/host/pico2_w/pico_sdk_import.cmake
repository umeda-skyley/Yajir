# Locate the Pico SDK installed by the VS Code extension or supplied explicitly.
if(NOT DEFINED PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR
        "PICO_SDK_PATH is not set. Configure with -DPICO_SDK_PATH=<path-to-pico-sdk> "
        "or use the Raspberry Pi Pico VS Code extension.")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
