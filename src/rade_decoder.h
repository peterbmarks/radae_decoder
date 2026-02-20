#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include "audio_stream.h"

/* Forward declaration — avoids exposing RADE/FARGAN C headers in this header */
struct rade;

/* ── RadaeDecoder ──────────────────────────────────────────────────────────
 *
 *  Real-time RADAE decoder pipeline:
 *    PortAudio capture → resample → Hilbert → RADE Rx → FARGAN → resample → PortAudio playback
 *
 *  All processing runs on a dedicated thread.  Status is exposed via atomics.
 * ──────────────────────────────────────────────────────────────────────── */

class RadaeDecoder {
public:
    RadaeDecoder();
    ~RadaeDecoder();

    /* lifecycle -------------------------------------------------------------- */
    bool open(const std::string& input_hw_id, const std::string& output_hw_id);
    bool open_file(const std::string& wav_path, const std::string& output_hw_id);
    void close();
    void start();
    void stop();

    /* status queries (thread-safe) ------------------------------------------ */
    bool  is_running()            const { return running_.load(std::memory_order_relaxed); }
    bool  is_synced()             const { return synced_.load(std::memory_order_relaxed); }
    float snr_dB()                const { return snr_dB_.load(std::memory_order_relaxed); }
    float freq_offset()           const { return freq_offset_.load(std::memory_order_relaxed); }
    float get_input_level()       const { return input_level_.load(std::memory_order_relaxed); }
    float get_output_level_left() const { return output_level_.load(std::memory_order_relaxed); }
    float get_output_level_right()const { return output_level_.load(std::memory_order_relaxed); } // mono

    /* spectrum (thread-safe via mutex) --------------------------------------- */
    static constexpr int FFT_SIZE      = 512;
    static constexpr int SPECTRUM_BINS = FFT_SIZE / 2;   // 256

    void get_spectrum(float* out, int n) const;           // copies up to n bins (dB)
    int  spectrum_bins()          const { return SPECTRUM_BINS; }
    float spectrum_sample_rate()  const { return 8000.f; } // always at modem rate

    /* callsign (thread-safe via mutex) --------------------------------------- */
    std::string last_callsign() const;

private:
    void processing_loop();

    /* ── audio stream handles ────────────────────────────────────────────── */
    AudioStream  stream_in_;
    AudioStream  stream_out_;
    unsigned int rate_in_  = 0;   // capture rate
    unsigned int rate_out_ = 0;   // playback rate

    /* ── RADE receiver (opaque) ───────────────────────────────────────────── */
    struct rade*  rade_     = nullptr;

    /* ── FARGAN vocoder (opaque void* to avoid C header in .h) ────────────── */
    void*         fargan_   = nullptr;

    /* ── Hilbert transform (127-tap FIR) ──────────────────────────────────── */
    static constexpr int HILBERT_NTAPS = 127;
    static constexpr int HILBERT_DELAY = (HILBERT_NTAPS - 1) / 2;  /* 63 */
    float hilbert_coeffs_[HILBERT_NTAPS] = {};
    float hilbert_hist_[HILBERT_NTAPS]   = {};   // history for FIR
    int   hilbert_pos_                   = 0;    // write position in history

    /* ── FARGAN warmup state ──────────────────────────────────────────────── */
    static constexpr int NB_TOTAL_FEAT = 36;
    bool  fargan_ready_    = false;
    int   warmup_count_    = 0;
    float warmup_buf_[5 * 36] = {};   // 5 frames × NB_TOTAL_FEATURES

    /* ── Resampler state (input) ──────────────────────────────────────────── */
    double resamp_in_frac_ = 0.0;   // fractional sample position for input resampler
    float  resamp_in_prev_ = 0.0f;  // previous input sample for linear interpolation

    /* ── Delay buffer for Hilbert real part ────────────────────────────────── */
    float delay_buf_[HILBERT_NTAPS] = {};
    int   delay_pos_                = 0;

    /* ── FFT / spectrum ────────────────────────────────────────────────────── */
    float              fft_window_[FFT_SIZE]      = {};
    float              spectrum_mag_[SPECTRUM_BINS] = {};   // dB magnitudes
    mutable std::mutex spectrum_mutex_;

    /* ── EOO callsign ───────────────────────────────────────────────────────── */
    std::string        last_callsign_;
    mutable std::mutex callsign_mutex_;

    /* ── Thread & atomics ─────────────────────────────────────────────────── */
    std::thread        thread_;
    std::atomic<bool>  running_     {false};
    std::atomic<bool>  synced_      {false};
    std::atomic<float> snr_dB_      {0.0f};
    std::atomic<float> freq_offset_ {0.0f};
    std::atomic<float> input_level_ {0.0f};
    std::atomic<float> output_level_{0.0f};

    /* ── File playback mode ────────────────────────────────────────────── */
    bool                file_mode_      = false;
    std::vector<float>  file_audio_8k_;          // pre-loaded 8 kHz mono audio
    size_t              file_pos_       = 0;      // read position in file_audio_8k_
};
