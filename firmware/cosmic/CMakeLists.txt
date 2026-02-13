cmake_minimum_required(VERSION 3.13)

# Export compile commands for LSP/clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Set board type to Pico 2 W (must be before pico_sdk_import)
set(PICO_BOARD pico2_w CACHE STRING "Board type")

# Import the Pico SDK from the path provided in the environment
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(CosmicUnicorn C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Pico SDK
pico_sdk_init()

# Full build with Lua shader support
add_executable(cosmic-full
    src/main.cpp
    src/board_config.cpp
    src/http_server.cpp
    src/shader_lua.cpp
    # Pimoroni libraries
    thirdparty/pimoroni/cosmic_unicorn.cpp
    thirdparty/pimoroni/pico_graphics.cpp
    thirdparty/pimoroni/pico_graphics_pen_rgb888.cpp
    thirdparty/pimoroni/pico_synth.cpp
    thirdparty/pimoroni/pimoroni_common.cpp
    thirdparty/pimoroni/bitmap_fonts/bitmap_fonts.cpp
    # Lua 5.4 source files
    thirdparty/lua/lapi.c
    thirdparty/lua/lauxlib.c
    thirdparty/lua/lbaselib.c
    thirdparty/lua/lcode.c
    thirdparty/lua/lcorolib.c
    thirdparty/lua/lctype.c
    thirdparty/lua/ldblib.c
    thirdparty/lua/ldebug.c
    thirdparty/lua/ldo.c
    thirdparty/lua/ldump.c
    thirdparty/lua/lfunc.c
    thirdparty/lua/lgc.c
    thirdparty/lua/linit.c
    thirdparty/lua/llex.c
    thirdparty/lua/lmathlib.c
    thirdparty/lua/lmem.c
    thirdparty/lua/lobject.c
    thirdparty/lua/lopcodes.c
    thirdparty/lua/lparser.c
    thirdparty/lua/lstate.c
    thirdparty/lua/lstring.c
    thirdparty/lua/lstrlib.c
    thirdparty/lua/ltable.c
    thirdparty/lua/ltablib.c
    thirdparty/lua/ltm.c
    thirdparty/lua/lundump.c
    thirdparty/lua/lutf8lib.c
    thirdparty/lua/lvm.c
    thirdparty/lua/lzio.c
)

# Generate PIO headers from vendored .pio files
pico_generate_pio_header(cosmic-full ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni/cosmic_unicorn.pio)
pico_generate_pio_header(cosmic-full ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni/audio_i2s.pio)

# Generate shader_editor_html.hpp from HTML file at build time
set(SHADER_EDITOR_HTML_INPUT ${CMAKE_CURRENT_SOURCE_DIR}/shader_editor.html)
set(SHADER_EDITOR_HTML_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/shader_editor_html.hpp)

add_custom_command(
    OUTPUT ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "// Auto-generated from shader_editor.html - do not edit" > ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "#pragma once" >> ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "#include <cstddef>" >> ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "static const char SHADER_EDITOR_HTML[] = R\"HTMLRAW(" >> ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E cat ${SHADER_EDITOR_HTML_INPUT} >> ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo ")HTMLRAW\";" >> ${SHADER_EDITOR_HTML_OUTPUT}
    COMMAND ${CMAKE_COMMAND} -E echo "static const size_t SHADER_EDITOR_HTML_LEN = sizeof(SHADER_EDITOR_HTML) - 1;" >> ${SHADER_EDITOR_HTML_OUTPUT}
    DEPENDS ${SHADER_EDITOR_HTML_INPUT}
    COMMENT "Generating shader_editor_html.hpp from HTML"
    VERBATIM
)

add_custom_target(generate_shader_editor_html DEPENDS ${SHADER_EDITOR_HTML_OUTPUT})
add_dependencies(cosmic-full generate_shader_editor_html)

# Include directory for lwipopts.h, lua, pimoroni, and generated headers
target_include_directories(cosmic-full PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/lua
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Link with the standard Pico SDK libraries
target_link_libraries(cosmic-full
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_flash
    hardware_pio
    hardware_dma
    hardware_gpio
    hardware_adc
    hardware_clocks
    hardware_irq
)

# Enable USB and UART output
pico_enable_stdio_usb(cosmic-full 1)
pico_enable_stdio_uart(cosmic-full 1)

# Create UF2, bin, and other output formats.
pico_add_extra_outputs(cosmic-full)

# --- Lite streaming-only build (no Lua/shaders/audio) ---
add_executable(cosmic-lite
    src/main_lite.cpp
    src/board_config.cpp
    src/http_server_lite.cpp
    # KCP reliable stream protocol (disabled - too much RAM)
    # thirdparty/ikcp.c
    # TJpgDec - Tiny JPEG Decoder
    thirdparty/tjpgd.c
    # Pimoroni libraries (pico_synth required by cosmic_unicorn DMA handler)
    # pico_graphics still needed: cosmic_unicorn.cpp::update() references PicoGraphics types
    thirdparty/pimoroni/cosmic_unicorn.cpp
    thirdparty/pimoroni/pico_graphics.cpp
    thirdparty/pimoroni/pico_graphics_pen_rgb888.cpp
    thirdparty/pimoroni/pico_synth.cpp
    thirdparty/pimoroni/pimoroni_common.cpp
)

pico_generate_pio_header(cosmic-lite ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni/cosmic_unicorn.pio)
pico_generate_pio_header(cosmic-lite ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni/audio_i2s.pio)

target_include_directories(cosmic-lite PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/pimoroni
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(cosmic-lite
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    hardware_flash
    hardware_pio
    hardware_dma
    hardware_gpio
    hardware_adc
    hardware_clocks
    hardware_irq
    hardware_sync
)

pico_enable_stdio_usb(cosmic-lite 1)
pico_enable_stdio_uart(cosmic-lite 1)
pico_add_extra_outputs(cosmic-lite)
