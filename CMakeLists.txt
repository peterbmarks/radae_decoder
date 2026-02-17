cmake_minimum_required(VERSION 3.16)
project(RADAE_Gui LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── external deps ────────────────────────────────────────────────────────────
find_package(PkgConfig  REQUIRED)
find_package(Threads    REQUIRED)

pkg_check_modules(GTK       REQUIRED gtk+-3.0)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

link_directories(${GTK_LIBRARY_DIRS} ${PORTAUDIO_LIBRARY_DIRS})

# ── Opus (with FARGAN/LPCNet support) ────────────────────────────────────────
include(cmake/BuildOpus.cmake)

# ── RADE library (Python-free) ───────────────────────────────────────────────
set(RADE_DSP_SOURCES
    src/rade_dsp.c
    src/rade_ofdm.c
    src/rade_bpf.c
    src/rade_acq.c
    src/rade_tx.c
    src/rade_rx.c
)

add_library(rade
    src/rade_api.c
    src/rade_enc.c
    src/rade_dec.c
    src/rade_enc_data.c
    src/rade_dec_data.c
    ${RADE_DSP_SOURCES}
)
target_link_libraries(rade opus m)
target_compile_definitions(rade PRIVATE -DIS_BUILDING_RADE_API=1 -DRADE_PYTHON_FREE=1)
target_include_directories(rade PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(rade PROPERTIES
    VERSION 0.1
    SOVERSION 0.1
    PUBLIC_HEADER "src/rade_api.h"
)

# ── RADE utilities ───────────────────────────────────────────────────────────
# Uncomment these to build the demo tools

# add_executable(lpcnet_demo src/tools/lpcnet_demo.c)
# target_link_libraries(lpcnet_demo opus m)

# add_executable(radae_tx src/tools/radae_tx.c)
# target_link_libraries(radae_tx rade opus m)

# add_executable(radae_rx src/tools/radae_rx.c)
# target_link_libraries(radae_rx rade opus m)

# add_executable(real2iq src/tools/real2iq.c)
# target_link_libraries(real2iq m)

# add_executable(rade_demod src/tools/rade_demod.c)
# target_link_libraries(rade_demod rade opus m)

# add_executable(rade_modulate src/tools/rade_modulate.c)
# target_link_libraries(rade_modulate rade opus m)

add_executable(webrx_rade_decode src/tools/webrx_rade_decode.c)
target_link_libraries(webrx_rade_decode rade opus m)

add_executable(radae_headless
    src/tools/radae_headless.cpp
    src/audio_input.cpp
    src/rade_decoder.cpp
    src/rade_encoder.cpp
)
target_link_libraries(radae_headless
    rade
    opus
    m
    ${PORTAUDIO_LIBRARIES}
    Threads::Threads
)
target_include_directories(radae_headless PRIVATE
    ${PORTAUDIO_INCLUDE_DIRS}
    ${OPUS_SOURCE_DIR}/dnn
    ${OPUS_SOURCE_DIR}/celt
    ${OPUS_SOURCE_DIR}/include
    ${OPUS_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── GUI application ──────────────────────────────────────────────────────────
add_executable(RADAE_Gui
    src/main.cpp
    src/audio_input.cpp
    src/meter_widget.cpp
    src/rade_decoder.cpp
    src/rade_encoder.cpp
    src/spectrum_widget.cpp
    src/waterfall_widget.cpp
)

target_include_directories(RADAE_Gui PRIVATE
    ${GTK_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${OPUS_SOURCE_DIR}/dnn
    ${OPUS_SOURCE_DIR}/celt
    ${OPUS_SOURCE_DIR}/include
    ${OPUS_SOURCE_DIR}
)

target_link_libraries(RADAE_Gui
    rade
    opus
    m
    ${GTK_LIBRARIES}
    ${PORTAUDIO_LIBRARIES}
    Threads::Threads
)

target_compile_options(RADAE_Gui PRIVATE
    ${GTK_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)
