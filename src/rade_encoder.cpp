#include "rade_encoder.h"

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

/* ── PulseAudio helper: open a pa_simple stream ──────────────────────── */

static pa_simple* open_pulse(const std::string& dev, pa_stream_direction_t dir,
                              unsigned int rate, const char* stream_name)
{
    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = rate;
    ss.channels = 1;

    pa_buffer_attr ba{};
    if (dir == PA_STREAM_RECORD) {
        ba.maxlength = static_cast<uint32_t>(-1);
        ba.fragsize  = 512 * 2;    // ~512 frames of S16
        ba.tlength   = static_cast<uint32_t>(-1);
        ba.prebuf    = static_cast<uint32_t>(-1);
        ba.minreq    = static_cast<uint32_t>(-1);
    } else {
        ba.maxlength = static_cast<uint32_t>(-1);
        ba.tlength   = 8192 * 2;   // ~8192 frames of S16
        ba.prebuf    = static_cast<uint32_t>(-1);
        ba.minreq    = static_cast<uint32_t>(-1);
        ba.fragsize  = static_cast<uint32_t>(-1);
    }

    int error = 0;
    pa_simple* pa = pa_simple_new(nullptr, "RADAE Encoder", dir,
                                  dev.c_str(), stream_name,
                                  &ss, nullptr, &ba, &error);
    return pa;
}

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

/* ── construction / destruction ──────────────────────────────────────── */

RadaeEncoder::RadaeEncoder()  = default;
RadaeEncoder::~RadaeEncoder() { stop(); close(); }

/* ── open / close ────────────────────────────────────────────────────── */

bool RadaeEncoder::open(const std::string& mic_hw_id,
                        const std::string& radio_hw_id)
{
    close();

    /* ── PulseAudio capture (mic, 16 kHz) ───────────────────────────── */
    rate_in_ = RADE_FS_SPEECH;
    pa_in_ = open_pulse(mic_hw_id, PA_STREAM_RECORD, rate_in_, "mic-capture");
    if (!pa_in_)
        return false;

    /* ── PulseAudio playback (radio, 8 kHz) ─────────────────────────── */
    rate_out_ = RADE_FS;
    pa_out_ = open_pulse(radio_hw_id, PA_STREAM_PLAYBACK, rate_out_, "modem-playback");
    if (!pa_out_) {
        pa_simple_free(pa_in_); pa_in_ = nullptr;
        return false;
    }

    /* ── RADE transmitter ────────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, RADE_VERBOSE_0);
    if (!rade_) {
        pa_simple_free(pa_in_);  pa_in_  = nullptr;
        pa_simple_free(pa_out_); pa_out_ = nullptr;
        return false;
    }

    /* ── LPCNet feature extractor ────────────────────────────────────── */
    lpcnet_ = lpcnet_encoder_create();
    if (!lpcnet_) {
        rade_close(rade_); rade_ = nullptr;
        pa_simple_free(pa_in_);  pa_in_  = nullptr;
        pa_simple_free(pa_out_); pa_out_ = nullptr;
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

    return true;
}

void RadaeEncoder::close()
{
    stop();

    if (rade_)   { rade_close(rade_);              rade_   = nullptr; }
    if (lpcnet_) { lpcnet_encoder_destroy(lpcnet_); lpcnet_ = nullptr; }

    if (pa_in_)  { pa_simple_free(pa_in_);  pa_in_  = nullptr; }
    if (pa_out_) { pa_simple_free(pa_out_); pa_out_ = nullptr; }

    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── start / stop ────────────────────────────────────────────────────── */

void RadaeEncoder::start()
{
    if (!pa_in_ || !pa_out_ || !rade_ || !lpcnet_ || running_) return;
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

/* ── helper: write IQ real part to PulseAudio output ─────────────────── */

static void write_real_to_pulse(pa_simple* pa, const RADE_COMP* iq, int n_iq,
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

    /* write to PulseAudio output */
    int error = 0;
    pa_simple_write(pa, out_pcm.data(),
                    static_cast<size_t>(n_resamp) * sizeof(int16_t), &error);
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

    /* ── Pre-fill output buffer with silence so the PulseAudio playback buffer
     *    has enough headroom to survive the ~120 ms gap between modem frame
     *    writes (each modem frame requires accumulating 12 feature frames
     *    of mic input before any output is produced). ──────────────────── */
    {
        const int prefill_frames = 2 * n_tx_out;              /* 2 modem frames */
        int prefill_out = prefill_frames
            * static_cast<int>(rate_out_) / static_cast<int>(RADE_FS);
        std::vector<int16_t> silence(static_cast<size_t>(prefill_out), 0);
        int error = 0;
        pa_simple_write(pa_out_, silence.data(),
                        silence.size() * sizeof(int16_t), &error);
    }

    while (running_.load(std::memory_order_relaxed)) {

        /* ── accumulate at least LPCNET_FRAME_SIZE (160) samples at 16 kHz ── */
        while (static_cast<int>(acc_16k.size()) < LPCNET_FRAME_SIZE &&
               running_.load(std::memory_order_relaxed))
        {
            int error = 0;
            int ret = pa_simple_read(pa_in_, capture_buf.data(),
                                     READ_FRAMES * sizeof(int16_t), &error);
            if (ret < 0) {
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
                write_real_to_pulse(pa_out_, tx_out.data(), n_out,
                                    RADE_FS, rate_out_,
                                    resamp_out_frac_, resamp_out_prev_,
                                    output_level_, running_,
                                    tx_scale_.load(std::memory_order_relaxed));
                feat_count = 0;
            }
        }
    }

    /* ── send end-of-over frame ──────────────────────────────────────── */
    if (rade_ && pa_out_) {
        int n_out = rade_tx_eoo(rade_, eoo_out.data());
        if (bpf_enabled_.load(std::memory_order_relaxed))
            rade_bpf_process(&bpf_, eoo_out.data(), eoo_out.data(), n_out);
        write_real_to_pulse(pa_out_, eoo_out.data(), n_out,
                            RADE_FS, rate_out_,
                            resamp_out_frac_, resamp_out_prev_,
                            output_level_, running_,
                            tx_scale_.load(std::memory_order_relaxed));
        int error = 0;
        pa_simple_drain(pa_out_, &error);
    }
}
