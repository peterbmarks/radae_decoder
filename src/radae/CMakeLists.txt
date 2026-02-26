# ── RADE library (Python-free) ───────────────────────────────────────────────
set(RADE_DSP_SOURCES
    rade_dsp.c
    rade_ofdm.c
    rade_bpf.c
    rade_acq.c
    rade_tx.c
    rade_rx.c
)

add_library(rade
    rade_api.c
    rade_enc.c
    rade_dec.c
    rade_enc_data.c
    rade_dec_data.c
    ${RADE_DSP_SOURCES}
)