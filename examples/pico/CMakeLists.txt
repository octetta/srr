cmake_minimum_required(VERSION 3.13)

# Include Pico SDK tools
include(pico_sdk_import.cmake OPTIONAL)

project(srr_pico_firmware C CXX ASM)
set(CMAKE_C_STANDARD 99)

if (NOT DEFINED PICO_SDK_PATH)
    message(STATUS "PICO_SDK_PATH not set. Skipping Pico hardware target build.")
    return()
endif()

pico_sdk_init()

add_executable(srr_pico_firmware
    pico_device_sim.c
    ../../src/srr_wire.c
    ../../src/srr_embedded.c
)

target_include_directories(srr_pico_firmware PRIVATE
    ../../include/srr
)

# Enable USB CDC or UART output
pico_enable_stdio_usb(srr_pico_firmware 1)
pico_enable_stdio_uart(srr_pico_firmware 1)

target_link_libraries(srr_pico_firmware pico_stdlib hardware_gpio hardware_adc)
pico_add_extra_outputs(srr_pico_firmware)
