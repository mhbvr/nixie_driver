cmake_minimum_required(VERSION 3.13)

# Board: pico2 (RP2350) by default. For the original Pico (RP2040) use:
#   cmake -DPICO_BOARD=pico ..
# NOTE: PICO_BOARD is stored in CMakeCache.txt. A build directory configured
# earlier for another board keeps that board - delete the build directory
# (or pass -DPICO_BOARD=...) when switching.
if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2)
endif()

include(pico_sdk_import.cmake)

project(nixie_tester C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

message(STATUS "nixie_tester: PICO_BOARD=${PICO_BOARD}, PICO_PLATFORM=${PICO_PLATFORM}")
if(NOT PICO_PLATFORM MATCHES "^rp2350")
    message(WARNING
        "Building for ${PICO_PLATFORM} (PICO_BOARD=${PICO_BOARD}), i.e. the original "
        "Pico. This UF2 will NOT run on a Pico 2: it drops straight back into "
        "BOOTSEL. For Pico 2 delete this build directory and re-run cmake, or "
        "pass -DPICO_BOARD=pico2.")
endif()

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

# CLI over USB CDC (virtual serial port) only; the UART is not used.
pico_enable_stdio_usb(nixie_tester 1)
pico_enable_stdio_uart(nixie_tester 0)

# Don't reboot into BOOTSEL when a host opens the port at 1200 baud (SDK
# default). Some serial tools/IDEs do that and the board would "fall" into the
# boot ROM drive. "picotool reboot -f -u" still works via the reset interface.
target_compile_definitions(nixie_tester PRIVATE
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
)

pico_add_extra_outputs(nixie_tester)
