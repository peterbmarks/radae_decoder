cmake_minimum_required(VERSION 3.16)
project(AudioLevelMeter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# ── external deps ────────────────────────────────────────────────────────────
find_package(PkgConfig  REQUIRED)
find_package(Threads    REQUIRED)

# ensure pkg-config can locate .pc files in the multiarch lib directory
set(ENV{PKG_CONFIG_PATH} "/usr/lib/x86_64-linux-gnu/pkgconfig")

pkg_check_modules(GTK  REQUIRED gtk+-3.0)
pkg_check_modules(ALSA REQUIRED alsa)

# ── RADAE (builds librade + opus with FARGAN/LPCNet) ─────────────────────────
add_subdirectory(radae_nopy)

# ── target ───────────────────────────────────────────────────────────────────
add_executable(audio_level_meter
    src/main.cpp
    src/audio_input.cpp
    src/meter_widget.cpp
    src/rade_decoder.cpp
    src/spectrum_widget.cpp
)

target_include_directories(audio_level_meter PRIVATE
    ${GTK_INCLUDE_DIRS}
    ${ALSA_INCLUDE_DIRS}
    ${OPUS_SOURCE_DIR}/dnn
    ${OPUS_SOURCE_DIR}/celt
    ${OPUS_SOURCE_DIR}/include
    ${OPUS_SOURCE_DIR}
)

target_link_libraries(audio_level_meter
    rade
    opus
    m
    ${GTK_LIBRARIES}
    ${ALSA_LIBRARIES}
    Threads::Threads
)

target_compile_options(audio_level_meter PRIVATE
    ${GTK_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)
