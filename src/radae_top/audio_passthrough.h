#pragma once

#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include "../src/audio/audio_stream.h"

/* ── AudioPassthrough ──────────────────────────────────────────────────────
 *
 *  Copies raw audio from a capture stream directly to a playback stream
 *  with no processing — "analog" / monitor mode.
 *
 *  Also computes input RMS level and FFT spectrum so the UI meters and
 *  waterfall keep working while in passthrough mode.
 *
 * ──────────────────────────────────────────────────────────────────────── */

class AudioPassthrough {
public:
    AudioPassthrough()  = default;
    ~AudioPassthrough() { stop(); close(); }

    AudioPassthrough(const AudioPassthrough&)            = delete;
    AudioPassthrough& operator=(const AudioPassthrough&) = delete;

    bool open(const std::string& input_hw_id,
              const std::string& output_hw_id);
    void close();
    void start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    /* ── UI status queries (thread-safe) ────────────────────────────────── */
    static constexpr int FFT_SIZE      = 512;
    static constexpr int SPECTRUM_BINS = FFT_SIZE / 2;

    float get_input_level() const { return input_level_.load(std::memory_order_relaxed); }
    void  get_spectrum(float* out, int n) const;
    int   spectrum_bins()        const { return SPECTRUM_BINS; }
    float spectrum_sample_rate() const { return 8000.f; }

private:
    void loop();

    AudioStream        stream_in_;
    AudioStream        stream_out_;
    std::thread        thread_;
    std::atomic<bool>  running_     {false};
    unsigned int       rate_        = 0;

    std::atomic<float> input_level_ {0.0f};
    float              spectrum_mag_[SPECTRUM_BINS] = {};
    mutable std::mutex spectrum_mutex_;
    float              fft_window_[FFT_SIZE]        = {};
};
