#include "rade_encoder.h"

#include <complex>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

/* ── C headers from RADE / Opus (wrapped for C++ linkage) ────────────── */
extern "C" {
#include "rade_api.h"
#include "rade_dsp.h"
#include "lpcnet.h"
#include "cpu_support.h"
}

#include "EooCallsignDecoder.hpp"

/* ── streaming linear-interpolation resampler (same as rade_decoder.cpp) */

static int resample_linear_stream(const float* in, int n_in,
                                  float* out, int max_out,
                                  unsigned int rate_in, unsigned int rate_out,
                                  double& frac, float& prev)
{
    if (rate_in == rate_out) {
        int n = std::min(n_in, max_out);
        std::memcpy(out, in, static_cast<size_t>(n) * sizeof(float));
        if (n_in > 0) prev = in[n_in - 1];
        return n;
    }

    double step = static_cast<double>(rate_in) / static_cast<double>(rate_out);
    int n_out = 0;

    while (n_out < max_out) {
        int idx = static_cast<int>(frac);
        if (idx >= n_in) break;

        float f = static_cast<float>(frac - idx);
        float s0 = (idx == 0) ? prev : in[idx - 1];
        float s1 = in[idx];
        out[n_out++] = s0 + f * (s1 - s0);

        frac += step;
    }

    if (n_in > 0) prev = in[n_in - 1];
    frac -= n_in;

    return n_out;
}

/* ── in-place radix-2 FFT ────────────────────────────────────────────── */

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

/* ── get_spectrum (thread-safe read) ─────────────────────────────────── */

void RadaeEncoder::get_spectrum(float* out, int n) const
{
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int count = std::min(n, SPECTRUM_BINS);
    std::memcpy(out, spectrum_mag_, static_cast<size_t>(count) * sizeof(float));
}

/* ── construction / destruction ──────────────────────────────────────── */

RadaeEncoder::RadaeEncoder()  = default;
RadaeEncoder::~RadaeEncoder() { stop(); close(); }

/* ── EOO callsign ────────────────────────────────────────────────────── */

void RadaeEncoder::set_callsign(const std::string& cs)
{
    callsign_ = cs;
    apply_callsign();
}

void RadaeEncoder::apply_callsign()
{
    if (!rade_) return;
    int n = rade_n_eoo_bits(rade_);
    std::vector<float> bits(static_cast<size_t>(n));
    EooCallsignDecoder enc;
    enc.encode(callsign_, bits.data(), n);
    rade_tx_set_eoo_bits(rade_, bits.data());
}

/* ── open / close ────────────────────────────────────────────────────── */

bool RadaeEncoder::open(const std::string& mic_hw_id,
                        const std::string& radio_hw_id)
{
    close();

    /* ── audio capture (mic, 16 kHz) ────────────────────────────────── */
    rate_in_ = RADE_FS_SPEECH;
    if (!stream_in_.open(mic_hw_id, true, 1, rate_in_, 160))
        return false;

    /* ── audio playback (radio, 8 kHz) ───────────────────────────────── */
    rate_out_ = RADE_FS;
    if (!stream_out_.open(radio_hw_id, false, 1, rate_out_, 512)) {
        stream_in_.close();
        return false;
    }

    /* ── RADE transmitter ────────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, RADE_VERBOSE_0);
    if (!rade_) {
        stream_in_.close();
        stream_out_.close();
        return false;
    }

    /* ── EOO callsign ────────────────────────────────────────────────── */
    apply_callsign();

    /* ── LPCNet feature extractor ────────────────────────────────────── */
    lpcnet_ = lpcnet_encoder_create();
    if (!lpcnet_) {
        rade_close(rade_); rade_ = nullptr;
        stream_in_.close();
        stream_out_.close();
        return false;
    }

    /* ── Resampler state ─────────────────────────────────────────────── */
    resamp_in_frac_  = 0.0;
    resamp_in_prev_  = 0.0f;
    resamp_out_frac_ = 0.0;
    resamp_out_prev_ = 0.0f;

    /* ── TX output bandpass filter (700–2300 Hz) ─────────────────────── */
    int n_eoo = rade_n_tx_eoo_out(rade_);
    rade_bpf_init(&bpf_, RADE_BPF_NTAP, static_cast<float>(RADE_FS),
                  1600.0f, 1500.0f, n_eoo);

    /* ── FFT window (Hann) for TX output spectrum ────────────────────── */
    for (int i = 0; i < FFT_SIZE; i++)
        fft_window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI)
                                                   * i / (FFT_SIZE - 1)));

    return true;
}

void RadaeEncoder::close()
{
    stop();

    if (rade_)   { rade_close(rade_);              rade_   = nullptr; }
    if (lpcnet_) { lpcnet_encoder_destroy(lpcnet_); lpcnet_ = nullptr; }

    stream_in_.close();
    stream_out_.close();

    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── start / stop ────────────────────────────────────────────────────── */

void RadaeEncoder::start()
{
    if (!stream_in_.is_open() || !stream_out_.is_open() || !rade_ || !lpcnet_ || running_) return;
    running_ = true;
    thread_  = std::thread(&RadaeEncoder::processing_loop, this);
}

void RadaeEncoder::stop()
{
    if (!running_) return;
    running_ = false;

    if (thread_.joinable()) thread_.join();

    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── helper: write IQ real part to audio output ──────────────────────── */

static void write_real_to_output(AudioStream& stream, const RADE_COMP* iq, int n_iq,
                                  unsigned int rate_modem, unsigned int rate_out,
                                  double& resamp_frac, float& resamp_prev,
                                  std::atomic<float>& output_level,
                                  const std::atomic<bool>& running,
                                  float tx_scale)
{
    (void)running;

    /* convert IQ → real float and compute RMS */
    std::vector<float> real_8k(static_cast<size_t>(n_iq));
    double rms_sum = 0.0;
    for (int i = 0; i < n_iq; i++) {
        real_8k[static_cast<size_t>(i)] = iq[i].real;
        rms_sum += static_cast<double>(iq[i].real) * iq[i].real;
    }
    output_level.store(std::sqrt(static_cast<float>(rms_sum / n_iq)),
                       std::memory_order_relaxed);

    /* resample 8 kHz → output device rate */
    int out_max = n_iq * static_cast<int>(rate_out) / static_cast<int>(rate_modem) + 4;
    std::vector<float> out_f(static_cast<size_t>(out_max));
    int n_resamp = resample_linear_stream(
        real_8k.data(), n_iq,
        out_f.data(), out_max,
        rate_modem, rate_out,
        resamp_frac, resamp_prev);

    /* float → S16 with caller-supplied scale */
    std::vector<int16_t> out_pcm(static_cast<size_t>(n_resamp));
    for (int s = 0; s < n_resamp; s++) {
        float v = out_f[static_cast<size_t>(s)] * tx_scale;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32767.0f) v = -32767.0f;
        out_pcm[static_cast<size_t>(s)] = static_cast<int16_t>(v);
    }

    /* write to audio output */
    stream.write(out_pcm.data(),
                 static_cast<unsigned long>(n_resamp));
}

/* ── processing loop (dedicated thread) ──────────────────────────────── */

void RadaeEncoder::processing_loop()
{
    int arch = opus_select_arch();

    int n_features_in = rade_n_features_in_out(rade_);   /* 432 */
    int n_tx_out      = rade_n_tx_out(rade_);            /* 960 */
    int n_eoo_out     = rade_n_tx_eoo_out(rade_);        /* 1152 */

    /* feature frames per modem frame: n_features_in / NB_TOTAL_FEATURES */
    int frames_per_modem = n_features_in / NB_TOTAL_FEATURES;   /* 12 */

    /* working buffers */
    std::vector<float>     features(static_cast<size_t>(n_features_in));
    std::vector<RADE_COMP> tx_out(static_cast<size_t>(n_tx_out));
    std::vector<RADE_COMP> eoo_out(static_cast<size_t>(n_eoo_out));

    int feat_count = 0;   /* how many feature frames accumulated */

    /* capture read buffer */
    constexpr int READ_FRAMES = 160;
    std::vector<int16_t> capture_buf(READ_FRAMES);

    /* accumulator for 16 kHz mono float samples */
    std::vector<float> acc_16k;
    acc_16k.reserve(1024);

    /* temporary buffer for resampled input */
    int resamp_out_max = READ_FRAMES + 2;
    std::vector<float> resamp_tmp(static_cast<size_t>(resamp_out_max));

    /* ── Pre-fill output buffer with silence so the PortAudio playback buffer
     *    has enough headroom to survive the ~120 ms gap between modem frame
     *    writes (each modem frame requires accumulating 12 feature frames
     *    of mic input before any output is produced). ──────────────────── */
    {
        const int prefill_frames = 2 * n_tx_out;              /* 2 modem frames */
        int prefill_out = prefill_frames
            * static_cast<int>(rate_out_) / static_cast<int>(RADE_FS);
        std::vector<int16_t> silence(static_cast<size_t>(prefill_out), 0);
        stream_out_.write(silence.data(),
                          static_cast<unsigned long>(prefill_out));
    }

    while (running_.load(std::memory_order_relaxed)) {

        /* ── accumulate at least LPCNET_FRAME_SIZE (160) samples at 16 kHz ── */
        while (static_cast<int>(acc_16k.size()) < LPCNET_FRAME_SIZE &&
               running_.load(std::memory_order_relaxed))
        {
            AudioError err = stream_in_.read(capture_buf.data(), READ_FRAMES);
            if (err == AUDIO_ERROR) {
                if (!running_.load(std::memory_order_relaxed)) break;
                continue;
            }

            int n = READ_FRAMES;

            /* convert S16 → float, apply mic gain */
            float gain = mic_gain_.load(std::memory_order_relaxed);
            std::vector<float> f_in(static_cast<size_t>(n));
            for (int i = 0; i < n; i++)
                f_in[static_cast<size_t>(i)] = capture_buf[static_cast<size_t>(i)] / 32768.0f * gain;

            /* resample to 16 kHz if needed */
            int got = resample_linear_stream(
                f_in.data(), n,
                resamp_tmp.data(), resamp_out_max,
                rate_in_, RADE_FS_SPEECH,
                resamp_in_frac_, resamp_in_prev_);

            acc_16k.insert(acc_16k.end(), resamp_tmp.begin(),
                           resamp_tmp.begin() + got);
        }

        if (!running_.load(std::memory_order_relaxed)) break;

        /* ── process complete 160-sample frames ──────────────────────────── */
        while (static_cast<int>(acc_16k.size()) >= LPCNET_FRAME_SIZE) {

            /* input RMS level (of this 10 ms frame) */
            {
                double sum2 = 0.0;
                for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
                    float s = acc_16k[static_cast<size_t>(i)];
                    sum2 += static_cast<double>(s) * s;
                }
                input_level_.store(std::sqrt(static_cast<float>(sum2 / LPCNET_FRAME_SIZE)),
                                   std::memory_order_relaxed);
            }

            /* convert float → int16 for LPCNet */
            int16_t pcm_frame[LPCNET_FRAME_SIZE];
            for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
                float v = acc_16k[static_cast<size_t>(i)] * 32768.0f;
                if (v >  32767.0f) v =  32767.0f;
                if (v < -32767.0f) v = -32767.0f;
                pcm_frame[i] = static_cast<int16_t>(v);
            }

            /* extract features */
            float frame_features[NB_TOTAL_FEATURES];
            lpcnet_compute_single_frame_features(lpcnet_, pcm_frame,
                                                  frame_features, arch);

            /* append to modem frame feature buffer */
            std::memcpy(&features[static_cast<size_t>(feat_count * NB_TOTAL_FEATURES)],
                        frame_features,
                        static_cast<size_t>(NB_TOTAL_FEATURES) * sizeof(float));
            feat_count++;

            /* consume 160 samples */
            acc_16k.erase(acc_16k.begin(),
                          acc_16k.begin() + LPCNET_FRAME_SIZE);

            /* ── full modem frame: encode and output ─────────────────────── */
            if (feat_count >= frames_per_modem) {
                int n_out = rade_tx(rade_, tx_out.data(), features.data());
                if (bpf_enabled_.load(std::memory_order_relaxed))
                    rade_bpf_process(&bpf_, tx_out.data(), tx_out.data(), n_out);

                /* FFT spectrum of TX output (real part, last FFT_SIZE samples) */
                if (n_out >= FFT_SIZE) {
                    std::complex<float> fft_buf[FFT_SIZE];
                    int off = n_out - FFT_SIZE;
                    for (int i = 0; i < FFT_SIZE; i++)
                        fft_buf[i] = tx_out[static_cast<size_t>(off + i)].real
                                     * fft_window_[i];
                    fft_radix2(fft_buf, FFT_SIZE);
                    float tmp[SPECTRUM_BINS];
                    for (int i = 0; i < SPECTRUM_BINS; i++) {
                        float mag = std::abs(fft_buf[i]) / (FFT_SIZE * 0.5f);
                        tmp[i] = (mag > 1e-10f)
                               ? 20.0f * std::log10(mag) : -200.0f;
                    }
                    {
                        std::lock_guard<std::mutex> lock(spectrum_mutex_);
                        std::memcpy(spectrum_mag_, tmp, sizeof(spectrum_mag_));
                    }
                }

                write_real_to_output(stream_out_, tx_out.data(), n_out,
                                     RADE_FS, rate_out_,
                                     resamp_out_frac_, resamp_out_prev_,
                                     output_level_, running_,
                                     tx_scale_.load(std::memory_order_relaxed));
                feat_count = 0;
            }
        }
    }

    /* ── send end-of-over frame ──────────────────────────────────────── */
    if (rade_ && stream_out_.is_open()) {
        int n_out = rade_tx_eoo(rade_, eoo_out.data());
        if (bpf_enabled_.load(std::memory_order_relaxed))
            rade_bpf_process(&bpf_, eoo_out.data(), eoo_out.data(), n_out);
        write_real_to_output(stream_out_, eoo_out.data(), n_out,
                             RADE_FS, rate_out_,
                             resamp_out_frac_, resamp_out_prev_,
                             output_level_, running_,
                             tx_scale_.load(std::memory_order_relaxed));
        /* drain the output buffer */
        stream_out_.stop();
        stream_out_.start();
    }
}
