#include "audio_passthrough.h"

#include <cmath>
#include <complex>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Use 8 kHz — matches the radio interface rate used by the RADE decoder,
   so the same device selection works for both modes. */
static constexpr unsigned int PASSTHROUGH_RATE   = 8000;
static constexpr int          PASSTHROUGH_FRAMES = 512;

/* ── radix-2 Cooley-Tukey FFT (in-place, power-of-two N) ────────────────── */
static void fft_radix2(std::complex<float>* x, int N)
{
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / len;
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                auto u = x[i + j];
                auto v = x[i + j + len / 2] * w;
                x[i + j]           = u + v;
                x[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

bool AudioPassthrough::open(const std::string& input_hw_id,
                            const std::string& output_hw_id)
{
    close();

    rate_ = PASSTHROUGH_RATE;

    /* Pre-compute Hanning window */
    for (int i = 0; i < FFT_SIZE; i++)
        fft_window_[i] = 0.5f * (1.0f - std::cos(
            2.0f * static_cast<float>(M_PI) * i / (FFT_SIZE - 1)));
    std::memset(spectrum_mag_, 0, sizeof(spectrum_mag_));

    if (!stream_in_.open(input_hw_id, true, 1, rate_, PASSTHROUGH_FRAMES))
        return false;

    if (!stream_out_.open(output_hw_id, false, 1, rate_, PASSTHROUGH_FRAMES)) {
        stream_in_.close();
        return false;
    }

    return true;
}

void AudioPassthrough::close()
{
    stop();
    stream_in_.close();
    stream_out_.close();
    rate_        = 0;
    input_level_.store(0.0f, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(spectrum_mutex_);
    std::memset(spectrum_mag_, 0, sizeof(spectrum_mag_));
}

void AudioPassthrough::start()
{
    if (!stream_in_.is_open() || !stream_out_.is_open() || running_) return;
    running_ = true;
    thread_  = std::thread(&AudioPassthrough::loop, this);
}

void AudioPassthrough::stop()
{
    running_ = false;
    if (thread_.joinable()) thread_.join();
    input_level_.store(0.0f, std::memory_order_relaxed);
}

void AudioPassthrough::get_spectrum(float* out, int n) const
{
    std::lock_guard<std::mutex> lk(spectrum_mutex_);
    int count = std::min(n, SPECTRUM_BINS);
    std::memcpy(out, spectrum_mag_, static_cast<size_t>(count) * sizeof(float));
}

void AudioPassthrough::loop()
{
    int16_t buf[PASSTHROUGH_FRAMES];

    /* Ring buffer of recent float samples for FFT — we accumulate a full
       FFT_SIZE window even though we read PASSTHROUGH_FRAMES at a time. */
    float   ring[FFT_SIZE] = {};
    int     ring_pos       = 0;

    /* Flush stale audio buffered before the stream started. */
    stream_in_.stop();

    while (running_.load(std::memory_order_relaxed)) {
        if (stream_in_.read(buf, PASSTHROUGH_FRAMES) != AUDIO_OK) continue;
        stream_out_.write(buf, PASSTHROUGH_FRAMES);

        /* ── convert to float and compute RMS ─────────────────────────── */
        float f[PASSTHROUGH_FRAMES];
        double sum2 = 0.0;
        for (int i = 0; i < PASSTHROUGH_FRAMES; i++) {
            f[i]  = buf[i] / 32768.0f;
            sum2 += static_cast<double>(f[i]) * f[i];
        }
        input_level_.store(
            static_cast<float>(std::sqrt(sum2 / PASSTHROUGH_FRAMES)),
            std::memory_order_relaxed);

        /* ── append samples to ring buffer ────────────────────────────── */
        for (int i = 0; i < PASSTHROUGH_FRAMES; i++) {
            ring[ring_pos] = f[i];
            ring_pos = (ring_pos + 1) % FFT_SIZE;
        }

        /* ── FFT over the latest FFT_SIZE samples ─────────────────────── */
        std::complex<float> fft_buf[FFT_SIZE];
        for (int i = 0; i < FFT_SIZE; i++) {
            int idx = (ring_pos + i) % FFT_SIZE;   // oldest-first ordering
            fft_buf[i] = ring[idx] * fft_window_[i];
        }

        fft_radix2(fft_buf, FFT_SIZE);

        float tmp[SPECTRUM_BINS];
        for (int i = 0; i < SPECTRUM_BINS; i++) {
            float mag = std::abs(fft_buf[i]) / (FFT_SIZE * 0.5f);
            tmp[i] = (mag > 1e-10f) ? 20.0f * std::log10(mag) : -200.0f;
        }
        {
            std::lock_guard<std::mutex> lk(spectrum_mutex_);
            std::memcpy(spectrum_mag_, tmp, sizeof(spectrum_mag_));
        }
    }
}
