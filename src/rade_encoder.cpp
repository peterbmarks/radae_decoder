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

/* ── ALSA helper (same as rade_decoder.cpp) ──────────────────────────── */

static std::string to_plughw(const std::string& hw_id)
{
    if (hw_id.compare(0, 3, "hw:") == 0)
        return "plughw:" + hw_id.substr(3);
    return hw_id;
}

static bool open_alsa(snd_pcm_t** pcm, const std::string& hw_id,
                      snd_pcm_stream_t stream, unsigned int* rate,
                      snd_pcm_uframes_t period_frames,
                      snd_pcm_uframes_t buffer_frames)
{
    std::string dev = to_plughw(hw_id);
    if (snd_pcm_open(pcm, dev.c_str(), stream, 0) < 0)
        return false;

    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    if (snd_pcm_hw_params_any(*pcm, hw) < 0)                              goto fail;
    if (snd_pcm_hw_params_set_access(*pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0) goto fail;
    if (snd_pcm_hw_params_set_format(*pcm, hw, SND_PCM_FORMAT_S16_LE) < 0) goto fail;
    if (snd_pcm_hw_params_set_channels(*pcm, hw, 1) < 0)                  goto fail;
    if (snd_pcm_hw_params_set_rate_near(*pcm, hw, rate, nullptr) < 0)      goto fail;
    snd_pcm_hw_params_set_period_size_near(*pcm, hw, &period_frames, nullptr);
    snd_pcm_hw_params_set_buffer_size_near(*pcm, hw, &buffer_frames);
    if (snd_pcm_hw_params(*pcm, hw) < 0)                                  goto fail;
    if (snd_pcm_prepare(*pcm) < 0)                                        goto fail;
    return true;

fail:
    snd_pcm_close(*pcm);
    *pcm = nullptr;
    return false;
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

    /* ── ALSA capture (mic, prefer 16 kHz) ───────────────────────────── */
    rate_in_ = RADE_FS_SPEECH;
    if (!open_alsa(&pcm_in_, mic_hw_id, SND_PCM_STREAM_CAPTURE,
                   &rate_in_, 512, 4096))
        return false;

    /* ── ALSA playback (radio, prefer 8 kHz) ─────────────────────────── */
    rate_out_ = RADE_FS;
    if (!open_alsa(&pcm_out_, radio_hw_id, SND_PCM_STREAM_PLAYBACK,
                   &rate_out_, 512, 8192)) {
        snd_pcm_close(pcm_in_); pcm_in_ = nullptr;
        return false;
    }

    /* ── RADE transmitter ────────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, RADE_VERBOSE_0);
    if (!rade_) {
        snd_pcm_close(pcm_in_);  pcm_in_  = nullptr;
        snd_pcm_close(pcm_out_); pcm_out_ = nullptr;
        return false;
    }

    /* ── LPCNet feature extractor ────────────────────────────────────── */
    lpcnet_ = lpcnet_encoder_create();
    if (!lpcnet_) {
        rade_close(rade_); rade_ = nullptr;
        snd_pcm_close(pcm_in_);  pcm_in_  = nullptr;
        snd_pcm_close(pcm_out_); pcm_out_ = nullptr;
        return false;
    }

    /* ── Resampler state ─────────────────────────────────────────────── */
    resamp_in_frac_  = 0.0;
    resamp_in_prev_  = 0.0f;
    resamp_out_frac_ = 0.0;
    resamp_out_prev_ = 0.0f;

    return true;
}

void RadaeEncoder::close()
{
    stop();

    if (rade_)   { rade_close(rade_);              rade_   = nullptr; }
    if (lpcnet_) { lpcnet_encoder_destroy(lpcnet_); lpcnet_ = nullptr; }

    if (pcm_in_)  { snd_pcm_close(pcm_in_);  pcm_in_  = nullptr; }
    if (pcm_out_) { snd_pcm_close(pcm_out_); pcm_out_ = nullptr; }

    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── start / stop ────────────────────────────────────────────────────── */

void RadaeEncoder::start()
{
    if (!pcm_in_ || !pcm_out_ || !rade_ || !lpcnet_ || running_) return;
    running_ = true;
    thread_  = std::thread(&RadaeEncoder::processing_loop, this);
}

void RadaeEncoder::stop()
{
    if (!running_) return;
    running_ = false;

    if (pcm_in_)  snd_pcm_drop(pcm_in_);
    if (pcm_out_) snd_pcm_drop(pcm_out_);

    if (thread_.joinable()) thread_.join();

    input_level_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── helper: write IQ real part to ALSA output ───────────────────────── */

static void write_real_to_alsa(snd_pcm_t* pcm, const RADE_COMP* iq, int n_iq,
                               unsigned int rate_modem, unsigned int rate_out,
                               double& resamp_frac, float& resamp_prev,
                               std::atomic<float>& output_level,
                               const std::atomic<bool>& running)
{
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

    /* float → S16 with scaling (16384 for moderate level with headroom) */
    static constexpr float TX_SCALE = 16384.0f;
    std::vector<int16_t> out_pcm(static_cast<size_t>(n_resamp));
    for (int s = 0; s < n_resamp; s++) {
        float v = out_f[static_cast<size_t>(s)] * TX_SCALE;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32767.0f) v = -32767.0f;
        out_pcm[static_cast<size_t>(s)] = static_cast<int16_t>(v);
    }

    /* write to ALSA output */
    int remaining = n_resamp;
    int16_t* ptr = out_pcm.data();
    while (remaining > 0 && running.load(std::memory_order_relaxed)) {
        snd_pcm_sframes_t written = snd_pcm_writei(
            pcm, ptr, static_cast<snd_pcm_uframes_t>(remaining));
        if (written < 0) {
            written = snd_pcm_recover(pcm, static_cast<int>(written), 1);
            if (written < 0) break;
            continue;
        }
        remaining -= static_cast<int>(written);
        ptr       += written;
    }
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
    constexpr snd_pcm_uframes_t READ_FRAMES = 160;
    std::vector<int16_t> capture_buf(READ_FRAMES);

    /* accumulator for 16 kHz mono float samples */
    std::vector<float> acc_16k;
    acc_16k.reserve(1024);

    /* temporary buffer for resampled input */
    int resamp_out_max = static_cast<int>(READ_FRAMES) + 2;
    std::vector<float> resamp_tmp(static_cast<size_t>(resamp_out_max));

    /* ── Pre-fill output buffer with silence so the ALSA playback buffer
     *    has enough headroom to survive the ~120 ms gap between modem frame
     *    writes (each modem frame requires accumulating 12 feature frames
     *    of mic input before any output is produced). ──────────────────── */
    {
        const int prefill_frames = 2 * n_tx_out;              /* 2 modem frames */
        int prefill_out = prefill_frames
            * static_cast<int>(rate_out_) / static_cast<int>(RADE_FS);
        std::vector<int16_t> silence(static_cast<size_t>(prefill_out), 0);
        int rem = prefill_out;
        int16_t* p = silence.data();
        while (rem > 0) {
            snd_pcm_sframes_t w = snd_pcm_writei(
                pcm_out_, p, static_cast<snd_pcm_uframes_t>(rem));
            if (w < 0) {
                w = snd_pcm_recover(pcm_out_, static_cast<int>(w), 1);
                if (w < 0) break;
                continue;
            }
            rem -= static_cast<int>(w);
            p   += w;
        }
    }

    while (running_.load(std::memory_order_relaxed)) {

        /* ── accumulate at least LPCNET_FRAME_SIZE (160) samples at 16 kHz ── */
        while (static_cast<int>(acc_16k.size()) < LPCNET_FRAME_SIZE &&
               running_.load(std::memory_order_relaxed))
        {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm_in_, capture_buf.data(), READ_FRAMES);
            if (n < 0) {
                if (!running_.load(std::memory_order_relaxed)) break;
                if (n == -EINTR) continue;
                n = snd_pcm_recover(pcm_in_, static_cast<int>(n), 1);
                if (n < 0) { running_ = false; break; }
                continue;
            }
            if (n == 0) continue;

            /* convert S16 → float */
            std::vector<float> f_in(static_cast<size_t>(n));
            for (snd_pcm_sframes_t i = 0; i < n; i++)
                f_in[static_cast<size_t>(i)] = capture_buf[static_cast<size_t>(i)] / 32768.0f;

            /* resample to 16 kHz if needed */
            int got = resample_linear_stream(
                f_in.data(), static_cast<int>(n),
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
                write_real_to_alsa(pcm_out_, tx_out.data(), n_out,
                                   RADE_FS, rate_out_,
                                   resamp_out_frac_, resamp_out_prev_,
                                   output_level_, running_);
                feat_count = 0;
            }
        }
    }

    /* ── send end-of-over frame ──────────────────────────────────────── */
    if (rade_ && pcm_out_) {
        /* prepare ALSA for one more write after the drop in stop() hasn't
           happened yet (we're still inside the thread) */
        snd_pcm_prepare(pcm_out_);
        int n_out = rade_tx_eoo(rade_, eoo_out.data());
        write_real_to_alsa(pcm_out_, eoo_out.data(), n_out,
                           RADE_FS, rate_out_,
                           resamp_out_frac_, resamp_out_prev_,
                           output_level_, running_);
        snd_pcm_drain(pcm_out_);
    }
}
