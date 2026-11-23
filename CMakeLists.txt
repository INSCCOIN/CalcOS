#
# CalcOS firmware for the ClockworkPi PicoCalc (RP2350 / Pico 2 W).
#
#   export PICO_SDK_PATH=~/pico/pico-sdk
#   mkdir build && cd build
#   cmake -DPICO_BOARD=pico2_w ..
#   make -j
#
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
include(pico_sdk_import.cmake)

project(calcos C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# The clock now lives in the settings file on the SD card ('set cpu'), so
# there is nothing to choose at build time. The firmware starts at the
# part's own 150 MHz and only changes afterwards if the card says to.

add_executable(calcos
    src/num.c
    src/value.c
    src/parse.c
    src/eval.c
    src/builtins.c
    src/shell.c
    src/lineedit.c
    src/units.c
    src/store.c
    src/bignum.c
    src/runtime.c
    src/settings.c
    cc/emit.c
    cc/lex.c
    cc/gen.c
    platform/picocalc/plat_picocalc.c
    platform/gfx_shapes.c
    platform/scrollback.c
    platform/picocalc/lcd.c
    platform/picocalc/keyboard.c
    platform/picocalc/sdfat.c
    platform/picocalc/pagestore.c
    platform/font.c
)

target_include_directories(calcos PRIVATE
    platform src cc platform/picocalc)

target_compile_definitions(calcos PRIVATE
    PICO_FLASH_SIZE_BYTES=4194304
)

target_compile_options(calcos PRIVATE
    -O2 -Wall -Wextra -ffunction-sections -fdata-sections
)

# The Cortex-M33 FPU is single precision only, so every double is a software
# routine from libgcc. Compiled programs call those same routines by address,
# which is why the runtime table in src/runtime.c references them directly.
target_link_libraries(calcos
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_clocks
    hardware_vreg
    hardware_watchdog
    pico_multicore
    hardware_pwm
    hardware_flash
    hardware_sync
)

pico_enable_stdio_usb(calcos 1)
pico_enable_stdio_uart(calcos 0)

pico_add_extra_outputs(calcos)
