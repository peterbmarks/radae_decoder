#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <pulse/simple.h>
#include <pulse/error.h>

/* Forward declarations — avoids exposing C headers in this header */
struct rade;
struct LPCNetEncState;

extern "C" {
#include "rade_bpf.h"
}

/* ── RadaeEncoder ──────────────────────────────────────────────────────────
 *
 *  Real-time RADAE encoder pipeline:
 *    PulseAudio capture (mic 16 kHz) → LPCNet features → RADE Tx → real → PulseAudio playback (radio 8 kHz)
 *
 *  All processing runs on a dedicated thread.  Status is exposed via atomics.
 * ──────────────────────────────────────────────────────────────────────── */

class RadaeEncoder {
public:
    RadaeEncoder();
    ~RadaeEncoder();

    /* lifecycle -------------------------------------------------------------- */
    bool open(const std::string& mic_hw_id, const std::string& radio_hw_id);
    void close();
    void start();
    void stop();

    /* status queries (thread-safe) ------------------------------------------ */
    bool  is_running()       const { return running_.load(std::memory_order_relaxed); }
    float get_input_level()  const { return input_level_.load(std::memory_order_relaxed); }
    float get_output_level() const { return output_level_.load(std::memory_order_relaxed); }

    /* TX level controls (thread-safe) --------------------------------------- */
    void  set_tx_scale(float s)  { tx_scale_.store(s, std::memory_order_relaxed); }
    float get_tx_scale() const   { return tx_scale_.load(std::memory_order_relaxed); }
    void  set_mic_gain(float g)  { mic_gain_.store(g, std::memory_order_relaxed); }
    float get_mic_gain() const   { return mic_gain_.load(std::memory_order_relaxed); }

    /* TX output bandpass filter (700–2300 Hz) */
    void set_bpf_enabled(bool en) { bpf_enabled_.store(en, std::memory_order_relaxed); }
    bool get_bpf_enabled() const  { return bpf_enabled_.load(std::memory_order_relaxed); }

private:
    void processing_loop();

    /* ── PulseAudio handles ───────────────────────────────────────────────── */
    pa_simple*   pa_in_   = nullptr;    // capture (mic)
    pa_simple*   pa_out_  = nullptr;    // playback (radio)
    unsigned int rate_in_  = 0;          // capture rate
    unsigned int rate_out_ = 0;          // playback rate

    /* ── RADE transmitter (opaque) ────────────────────────────────────────── */
    struct rade*        rade_    = nullptr;
    LPCNetEncState*     lpcnet_  = nullptr;

    /* ── Resampler state (capture rate → 16 kHz) ─────────────────────────── */
    double resamp_in_frac_  = 0.0;
    float  resamp_in_prev_  = 0.0f;

    /* ── Resampler state (8 kHz → playback rate) ─────────────────────────── */
    double resamp_out_frac_ = 0.0;
    float  resamp_out_prev_ = 0.0f;

    /* ── Thread & atomics ─────────────────────────────────────────────────── */
    std::thread        thread_;
    std::atomic<bool>  running_      {false};
    std::atomic<float> input_level_  {0.0f};
    std::atomic<float> output_level_ {0.0f};
    std::atomic<float> tx_scale_     {16384.0f};
    std::atomic<float> mic_gain_     {1.0f};
    std::atomic<bool>  bpf_enabled_  {false};

    /* ── TX output bandpass filter ───────────────────────────────────────── */
    rade_bpf           bpf_;
};
