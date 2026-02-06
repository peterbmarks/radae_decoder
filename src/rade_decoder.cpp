#include "rade_decoder.h"

#include <cmath>
#include <cstring>
#include <complex>
#include <vector>
#include <algorithm>
#include <mutex>

/* ── C headers from RADE / Opus (wrapped for C++ linkage) ────────────── */
extern "C" {
#include "rade_api.h"
#include "rade_dsp.h"
#include "fargan.h"
#include "lpcnet.h"
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── construction / destruction ──────────────────────────────────────── */

RadaeDecoder::RadaeDecoder()  = default;
RadaeDecoder::~RadaeDecoder() { stop(); close(); }

/* ── Hilbert coefficient initialisation (matches rade_demod.c) ───────── */

static void init_hilbert_coeffs(float coeffs[], int ntaps) {
    int center = (ntaps - 1) / 2;
    for (int i = 0; i < ntaps; i++) {
        int n = i - center;
        if (n == 0 || (n & 1) == 0) {
            coeffs[i] = 0.0f;
        } else {
            float h = 2.0f / (static_cast<float>(M_PI) * n);
            float w = 0.54f - 0.46f * std::cos(2.0f * static_cast<float>(M_PI) * i / (ntaps - 1));
            coeffs[i] = h * w;
        }
    }
}

/* ── radix-2 Cooley-Tukey FFT (in-place, N must be power of 2) ────────── */

static void fft_radix2(std::complex<float>* x, int N)
{
    /* bit-reversal permutation */
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    /* butterfly passes */
    for (int len = 2; len <= N; len <<= 1) {
        float ang = -2.0f * static_cast<float>(M_PI) / len;
        std::complex<float> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < N; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; j++) {
                auto u = x[i + j];
                auto v = x[i + j + len / 2] * w;
                x[i + j]             = u + v;
                x[i + j + len / 2]   = u - v;
                w *= wlen;
            }
        }
    }
}

/* ── get_spectrum (thread-safe read) ─────────────────────────────────── */

void RadaeDecoder::get_spectrum(float* out, int n) const
{
    std::lock_guard<std::mutex> lock(spectrum_mutex_);
    int count = std::min(n, SPECTRUM_BINS);
    std::memcpy(out, spectrum_mag_, static_cast<size_t>(count) * sizeof(float));
}

/* ── ALSA helper: open a PCM device with desired params ──────────────── */

/* Convert "hw:X,Y" to "plughw:X,Y" so ALSA handles rate/channel/format
   conversion automatically.  Pass through anything else unchanged. */
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

/* ── open / close ────────────────────────────────────────────────────── */

bool RadaeDecoder::open(const std::string& input_hw_id,
                        const std::string& output_hw_id)
{
    close();

    /* ── ALSA capture (mono, prefer 8 kHz) ──────────────────────────── */
    rate_in_ = RADE_FS;
    if (!open_alsa(&pcm_in_, input_hw_id, SND_PCM_STREAM_CAPTURE,
                   &rate_in_, 512, 2048))
        return false;

    /* ── ALSA playback (mono, prefer 16 kHz) ────────────────────────── */
    rate_out_ = RADE_FS_SPEECH;
    if (!open_alsa(&pcm_out_, output_hw_id, SND_PCM_STREAM_PLAYBACK,
                   &rate_out_, 512, 4096)) {
        snd_pcm_close(pcm_in_); pcm_in_ = nullptr;
        return false;
    }

    /* ── RADE receiver ──────────────────────────────────────────────── */
    rade_initialize();
    rade_ = rade_open(nullptr, RADE_VERBOSE_0);
    if (!rade_) {
        snd_pcm_close(pcm_in_);  pcm_in_  = nullptr;
        snd_pcm_close(pcm_out_); pcm_out_ = nullptr;
        return false;
    }

    /* ── FARGAN vocoder ─────────────────────────────────────────────── */
    fargan_ = new FARGANState;
    fargan_init(static_cast<FARGANState*>(fargan_));
    fargan_ready_ = false;
    warmup_count_ = 0;

    /* ── Hilbert coefficients ───────────────────────────────────────── */
    init_hilbert_coeffs(hilbert_coeffs_, HILBERT_NTAPS);
    std::memset(hilbert_hist_, 0, sizeof(hilbert_hist_));
    hilbert_pos_ = 0;
    std::memset(delay_buf_, 0, sizeof(delay_buf_));
    delay_pos_ = 0;

    /* ── Resampler state ────────────────────────────────────────────── */
    resamp_in_frac_ = 0.0;
    resamp_in_prev_ = 0.0f;

    /* ── Hanning window for FFT ─────────────────────────────────────── */
    for (int i = 0; i < FFT_SIZE; i++)
        fft_window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (FFT_SIZE - 1)));
    std::memset(spectrum_mag_, 0, sizeof(spectrum_mag_));

    return true;
}

void RadaeDecoder::close()
{
    stop();

    if (rade_) { rade_close(rade_); rade_ = nullptr; }
    if (fargan_) { delete static_cast<FARGANState*>(fargan_); fargan_ = nullptr; }

    if (pcm_in_)  { snd_pcm_close(pcm_in_);  pcm_in_  = nullptr; }
    if (pcm_out_) { snd_pcm_close(pcm_out_); pcm_out_ = nullptr; }

    synced_       = false;
    snr_dB_       = 0.0f;
    freq_offset_  = 0.0f;
    output_level_ = 0.0f;
}

/* ── start / stop ────────────────────────────────────────────────────── */

void RadaeDecoder::start()
{
    if (!pcm_in_ || !pcm_out_ || !rade_ || running_) return;
    running_ = true;
    thread_  = std::thread(&RadaeDecoder::processing_loop, this);
}

void RadaeDecoder::stop()
{
    if (!running_) return;
    running_ = false;

    if (pcm_in_)  snd_pcm_drop(pcm_in_);   // unblock capture read
    if (pcm_out_) snd_pcm_drop(pcm_out_);   // unblock playback write

    if (thread_.joinable()) thread_.join();

    output_level_ = 0.0f;
    synced_       = false;
}

/* ── streaming Hilbert transform ─────────────────────────────────────
 *
 *  For each input sample at 8 kHz, produce one RADE_COMP:
 *    .real = sample delayed by HILBERT_DELAY (63 samples)
 *    .imag = Hilbert-filtered sample
 * ──────────────────────────────────────────────────────────────────── */

static void hilbert_process(const float* in, RADE_COMP* out, int n,
                            const float coeffs[], float hist[], int& pos,
                            float delay[], int& dpos, int ntaps, int delay_n)
{
    for (int i = 0; i < n; i++) {
        float sample = in[i];

        /* store in history ring buffer */
        hist[pos] = sample;

        /* FIR convolution for imaginary part */
        float imag = 0.0f;
        for (int k = 0; k < ntaps; k++) {
            int idx = pos - k;
            if (idx < 0) idx += ntaps;
            imag += coeffs[k] * hist[idx];
        }

        /* delayed real part */
        delay[dpos] = sample;
        int read_pos = dpos - delay_n;
        if (read_pos < 0) read_pos += ntaps;
        out[i].real = delay[read_pos];
        out[i].imag = imag;

        pos = (pos + 1) % ntaps;
        dpos = (dpos + 1) % ntaps;
    }
}

/* ── streaming linear-interpolation resampler ────────────────────────
 *
 *  Converts from rate_in to rate_out.  Maintains fractional position
 *  across calls via frac and prev.
 *  Returns number of output samples written.
 * ──────────────────────────────────────────────────────────────────── */

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

    /* save last sample for next block interpolation */
    if (n_in > 0) prev = in[n_in - 1];

    /* adjust frac so it's relative to the next block */
    frac -= n_in;

    return n_out;
}

/* ── processing loop (dedicated thread) ──────────────────────────────── */

void RadaeDecoder::processing_loop()
{
    int nin_max        = rade_nin_max(rade_);
    int n_features_out = rade_n_features_in_out(rade_);
    int n_eoo_bits     = rade_n_eoo_bits(rade_);

    /* allocate working buffers */
    std::vector<RADE_COMP> rx_buf(static_cast<size_t>(nin_max));
    std::vector<float>     feat_buf(static_cast<size_t>(n_features_out));
    std::vector<float>     eoo_buf(static_cast<size_t>(n_eoo_bits));

    /* accumulation buffer for 8 kHz mono float samples */
    std::vector<float> acc_8k;
    acc_8k.reserve(static_cast<size_t>(nin_max * 2));

    /* capture read buffer (S16_LE mono, at rate_in_) */
    constexpr snd_pcm_uframes_t READ_FRAMES = 512;
    std::vector<int16_t> capture_buf(READ_FRAMES);

    /* temporary buffer for resampled input */
    int resamp_out_max = static_cast<int>(READ_FRAMES) + 2;
    std::vector<float> resamp_tmp(static_cast<size_t>(resamp_out_max));

    /* output resample state (16 kHz → rate_out_) */
    double resamp_out_frac = 0.0;
    float  resamp_out_prev = 0.0f;

    bool was_synced = false;

    while (running_.load(std::memory_order_relaxed)) {

        int nin = rade_nin(rade_);

        /* ── accumulate enough 8 kHz samples ─────────────────────────── */
        while (static_cast<int>(acc_8k.size()) < nin &&
               running_.load(std::memory_order_relaxed))
        {
            snd_pcm_sframes_t n = snd_pcm_readi(pcm_in_, capture_buf.data(), READ_FRAMES);
            if (n < 0) {
                if (!running_.load(std::memory_order_relaxed)) break;
                if (n == -EINTR) continue;
                n = snd_pcm_recover(pcm_in_, static_cast<int>(n), 0);
                if (n < 0) { running_ = false; break; }
                continue;
            }
            if (n == 0) continue;

            /* convert S16 → float */
            std::vector<float> f_in(static_cast<size_t>(n));
            for (snd_pcm_sframes_t i = 0; i < n; i++)
                f_in[static_cast<size_t>(i)] = capture_buf[static_cast<size_t>(i)] / 32768.0f;

            /* resample to 8 kHz */
            int got = resample_linear_stream(
                f_in.data(), static_cast<int>(n),
                resamp_tmp.data(), resamp_out_max,
                rate_in_, RADE_FS,
                resamp_in_frac_, resamp_in_prev_);

            acc_8k.insert(acc_8k.end(), resamp_tmp.begin(),
                          resamp_tmp.begin() + got);
        }

        if (!running_.load(std::memory_order_relaxed)) break;

        /* ── FFT spectrum of input 8 kHz audio ───────────────────────── */
        if (static_cast<int>(acc_8k.size()) >= FFT_SIZE) {
            std::complex<float> fft_buf[FFT_SIZE];
            int offset = static_cast<int>(acc_8k.size()) - FFT_SIZE;
            for (int i = 0; i < FFT_SIZE; i++)
                fft_buf[i] = acc_8k[static_cast<size_t>(offset + i)] * fft_window_[i];

            fft_radix2(fft_buf, FFT_SIZE);

            float tmp[SPECTRUM_BINS];
            for (int i = 0; i < SPECTRUM_BINS; i++) {
                float mag = std::abs(fft_buf[i]) / (FFT_SIZE * 0.5f);
                tmp[i] = (mag > 1e-10f)
                       ? 20.0f * std::log10(mag)
                       : -200.0f;
            }
            {
                std::lock_guard<std::mutex> lock(spectrum_mutex_);
                std::memcpy(spectrum_mag_, tmp, sizeof(spectrum_mag_));
            }
        }

        /* ── Hilbert transform: real 8 kHz → complex IQ ──────────────── */
        hilbert_process(acc_8k.data(), rx_buf.data(), nin,
                        hilbert_coeffs_, hilbert_hist_, hilbert_pos_,
                        delay_buf_, delay_pos_, HILBERT_NTAPS, HILBERT_DELAY);

        /* consume nin samples from accumulator */
        acc_8k.erase(acc_8k.begin(), acc_8k.begin() + nin);

        /* ── RADE Rx ─────────────────────────────────────────────────── */
        int has_eoo = 0;
        int n_out = rade_rx(rade_, feat_buf.data(), &has_eoo,
                            eoo_buf.data(), rx_buf.data());

        /* update sync status */
        bool now_synced = (rade_sync(rade_) != 0);
        synced_.store(now_synced, std::memory_order_relaxed);

        if (now_synced) {
            snr_dB_.store(static_cast<float>(rade_snrdB_3k_est(rade_)),
                          std::memory_order_relaxed);
            freq_offset_.store(rade_freq_offset(rade_),
                               std::memory_order_relaxed);
        }

        /* handle sync transitions */
        if (was_synced && !now_synced) {
            /* lost sync — reset FARGAN for next sync */
            fargan_init(static_cast<FARGANState*>(fargan_));
            fargan_ready_ = false;
            warmup_count_ = 0;
        }
        was_synced = now_synced;

        /* ── synthesise decoded speech ───────────────────────────────── */
        if (n_out > 0) {
            int n_frames = n_out / RADE_NB_TOTAL_FEATURES;
            double rms_sum = 0.0;
            int    rms_n   = 0;

            for (int fi = 0; fi < n_frames; fi++) {
                float* feat = &feat_buf[static_cast<size_t>(fi * RADE_NB_TOTAL_FEATURES)];

                /* ── FARGAN warmup: buffer first 5 frames ─────────────── */
                if (!fargan_ready_) {
                    std::memcpy(&warmup_buf_[warmup_count_ * NB_TOTAL_FEAT],
                                feat,
                                static_cast<size_t>(NB_TOTAL_FEAT) * sizeof(float));

                    if (++warmup_count_ >= 5) {
                        /* pack to NB_FEATURES stride for fargan_cont */
                        float packed[5 * NB_FEATURES];
                        for (int i = 0; i < 5; i++)
                            std::memcpy(&packed[i * NB_FEATURES],
                                        &warmup_buf_[i * NB_TOTAL_FEAT],
                                        static_cast<size_t>(NB_FEATURES) * sizeof(float));

                        float zeros[FARGAN_CONT_SAMPLES] = {};
                        fargan_cont(static_cast<FARGANState*>(fargan_),
                                    zeros, packed);
                        fargan_ready_ = true;
                    }
                    continue;   /* warmup frames not synthesised */
                }

                /* ── synthesise one 10-ms speech frame ────────────────── */
                float fpcm[LPCNET_FRAME_SIZE];
                fargan_synthesize(static_cast<FARGANState*>(fargan_),
                                  fpcm, feat);

                /* accumulate RMS of output */
                for (int s = 0; s < LPCNET_FRAME_SIZE; s++)
                    rms_sum += static_cast<double>(fpcm[s]) * fpcm[s];
                rms_n += LPCNET_FRAME_SIZE;

                /* ── resample 16 kHz → output rate & write ────────────── */
                /* max output samples: LPCNET_FRAME_SIZE * (rate_out/16000) + 2 */
                int out_max = LPCNET_FRAME_SIZE * static_cast<int>(rate_out_) / RADE_FS_SPEECH + 4;
                std::vector<float> out_f(static_cast<size_t>(out_max));

                int n_resamp = resample_linear_stream(
                    fpcm, LPCNET_FRAME_SIZE,
                    out_f.data(), out_max,
                    RADE_FS_SPEECH, rate_out_,
                    resamp_out_frac, resamp_out_prev);

                /* float → S16 */
                std::vector<int16_t> out_pcm(static_cast<size_t>(n_resamp));
                for (int s = 0; s < n_resamp; s++) {
                    float v = out_f[static_cast<size_t>(s)] * 32768.0f;
                    if (v >  32767.0f) v =  32767.0f;
                    if (v < -32767.0f) v = -32767.0f;
                    out_pcm[static_cast<size_t>(s)] = static_cast<int16_t>(
                        std::floor(0.5 + static_cast<double>(v)));
                }

                /* write to ALSA output */
                snd_pcm_sframes_t written = 0;
                int remaining = n_resamp;
                int16_t* ptr = out_pcm.data();
                while (remaining > 0 && running_.load(std::memory_order_relaxed)) {
                    written = snd_pcm_writei(pcm_out_, ptr,
                                             static_cast<snd_pcm_uframes_t>(remaining));
                    if (written < 0) {
                        written = snd_pcm_recover(pcm_out_,
                                                  static_cast<int>(written), 0);
                        if (written < 0) break;
                        continue;
                    }
                    remaining -= static_cast<int>(written);
                    ptr       += written;
                }
            }

            /* update output level */
            if (rms_n > 0)
                output_level_.store(
                    static_cast<float>(std::sqrt(rms_sum / rms_n)),
                    std::memory_order_relaxed);
        } else {
            /* no decoded output this frame — decay level toward zero */
            float lvl = output_level_.load(std::memory_order_relaxed);
            output_level_.store(lvl * 0.9f, std::memory_order_relaxed);
        }
    }
}
