#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <alsa/asoundlib.h>

/* Forward declarations — avoids exposing C headers in this header */
struct rade;
struct LPCNetEncState;

/* ── RadaeEncoder ──────────────────────────────────────────────────────────
 *
 *  Real-time RADAE encoder pipeline:
 *    ALSA capture (mic 16 kHz) → LPCNet features → RADE Tx → real → ALSA playback (radio 8 kHz)
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

private:
    void processing_loop();

    /* ── ALSA handles ─────────────────────────────────────────────────────── */
    snd_pcm_t*   pcm_in_   = nullptr;    // capture (mic)
    snd_pcm_t*   pcm_out_  = nullptr;    // playback (radio)
    unsigned int  rate_in_  = 0;          // negotiated capture rate
    unsigned int  rate_out_ = 0;          // negotiated playback rate

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
};
