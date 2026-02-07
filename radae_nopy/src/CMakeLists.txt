include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# LPCNet feature extraction tool
add_executable(lpcnet_demo lpcnet_demo.c)
target_link_libraries(lpcnet_demo opus m)

# DSP source files
set(RADE_DSP_SOURCES
    rade_dsp.c
    rade_ofdm.c
    rade_bpf.c
    rade_acq.c
    rade_tx.c
    rade_rx.c
)

# RADE library (Python-free)
add_library(rade
    rade_api_nopy.c
    rade_enc.c
    rade_dec.c
    rade_enc_data.c
    rade_dec_data.c
    ${RADE_DSP_SOURCES}
)
target_link_libraries(rade opus m)
target_compile_definitions(rade PRIVATE -DIS_BUILDING_RADE_API=1 -DRADE_PYTHON_FREE=1)
target_include_directories(rade PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(rade PROPERTIES
    VERSION 0.1
    SOVERSION 0.1
    PUBLIC_HEADER "rade_api.h"
)

# Standalone transmitter
add_executable(radae_tx radae_tx_nopy.c)
target_link_libraries(radae_tx rade opus m)

# Standalone receiver
add_executable(radae_rx radae_rx_nopy.c)
target_link_libraries(radae_rx rade opus m)

# Utility: convert real baseband to complex IQ
add_executable(real2iq real2iq.c)
target_link_libraries(real2iq m)

# WAV demodulator: WAV in → demod → WAV out
add_executable(rade_demod rade_demod.c)
target_link_libraries(rade_demod rade opus m)

# WAV modulator: WAV in → features → RADE encode → OFDM mod → WAV out
add_executable(rade_modulate rade_modulate.c)
target_link_libraries(rade_modulate rade opus m)
