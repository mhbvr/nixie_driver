cmake_minimum_required(VERSION 3.13)

# Board: pico (RP2040) by default. For Pico 2 use: cmake -DPICO_BOARD=pico2 ..
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico)
endif()

include(pico_sdk_import.cmake)

project(nixie_tester C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

option(NIXIE_STDIO_USB "Also expose the CLI on USB CDC (in addition to UART)" OFF)

add_executable(nixie_tester
    src/main.c
    src/outputs.c
)

target_compile_options(nixie_tester PRIVATE -Wall -Wextra)

target_link_libraries(nixie_tester
    pico_stdlib
    hardware_pwm
    hardware_clocks
)

# CLI on UART0: GP0 = TX, GP1 = RX, 115200 8N1
pico_enable_stdio_uart(nixie_tester 1)
if(NIXIE_STDIO_USB)
    pico_enable_stdio_usb(nixie_tester 1)
else()
    pico_enable_stdio_usb(nixie_tester 0)
endif()

pico_add_extra_outputs(nixie_tester)
