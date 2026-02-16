#include "audio_input.h"

#include <cmath>
#include <vector>

/* ── construction / destruction ──────────────────────────────────────────── */

AudioInput::AudioInput()  = default;
AudioInput::~AudioInput() { stop(); close(); }

/* ── device enumeration ─────────────────────────────────────────────────── */

std::vector<AudioDevice> AudioInput::enumerate_devices()
{
    std::vector<AudioDevice> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels < 1) continue;

        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        AudioDevice ad;
        ad.name = std::string(api ? api->name : "") + "  \xe2\x80\x94  "
                + (info->name ? info->name : "Unknown");
        ad.device_idx = i;
        devices.push_back(std::move(ad));
    }
    return devices;
}

std::vector<AudioDevice> AudioInput::enumerate_playback_devices()
{
    std::vector<AudioDevice> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels < 1) continue;

        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        AudioDevice ad;
        ad.name = std::string(api ? api->name : "") + "  \xe2\x80\x94  "
                + (info->name ? info->name : "Unknown");
        ad.device_idx = i;
        devices.push_back(std::move(ad));
    }
    return devices;
}

/* ── open / close ───────────────────────────────────────────────────────── */

bool AudioInput::open(int device_idx)
{
    close();                                // tidy up any previous handle

    const PaDeviceInfo* info = Pa_GetDeviceInfo(device_idx);
    if (!info) return false;

    /* prefer stereo, fall back to mono */
    channels_ = (info->maxInputChannels >= 2) ? 2 : 1;

    PaStreamParameters params{};
    params.device           = device_idx;
    params.channelCount     = channels_;
    params.sampleFormat     = paInt16;
    params.suggestedLatency = info->defaultLowInputLatency;

    PaError err = Pa_OpenStream(&stream_, &params, nullptr,
                                44100.0, 512, paClipOff,
                                nullptr, nullptr);
    if (err != paNoError) {
        stream_   = nullptr;
        channels_ = 0;
        return false;
    }

    return true;
}

void AudioInput::close()
{
    stop();
    if (stream_) {
        Pa_CloseStream(stream_);
        stream_   = nullptr;
        channels_ = 0;
    }
    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── start / stop ───────────────────────────────────────────────────────── */

void AudioInput::start()
{
    if (!stream_ || running_) return;
    Pa_StartStream(stream_);
    running_ = true;
    thread_  = std::thread(&AudioInput::capture_loop, this);
}

void AudioInput::stop()
{
    if (!running_) return;
    running_ = false;

    if (stream_) Pa_AbortStream(stream_);   // unblock a pending Pa_ReadStream()

    if (thread_.joinable()) thread_.join();

    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── capture loop (dedicated thread) ────────────────────────────────────── */

void AudioInput::capture_loop()
{
    constexpr unsigned long READ_FRAMES = 512;
    std::vector<int16_t> buf(READ_FRAMES * static_cast<unsigned>(channels_));

    while (running_.load(std::memory_order_relaxed)) {

        PaError err = Pa_ReadStream(stream_, buf.data(), READ_FRAMES);

        if (err != paNoError && err != paInputOverflowed) {
            if (!running_.load(std::memory_order_relaxed))
                break;
            continue;
        }

        long n = static_cast<long>(READ_FRAMES);

        /* ── per-channel RMS ───────────────────────────────────────── */
        double sum_l = 0.0, sum_r = 0.0;

        if (channels_ == 1) {
            for (long i = 0; i < n; ++i) {
                double s = buf[i] / 32768.0;
                sum_l   += s * s;
            }
            sum_r = sum_l;                  // duplicate mono → both channels
        } else {
            for (long i = 0; i < n; ++i) {
                double l = buf[i * 2]     / 32768.0;
                double r = buf[i * 2 + 1] / 32768.0;
                sum_l   += l * l;
                sum_r   += r * r;
            }
        }

        level_left_.store(static_cast<float>(std::sqrt(sum_l / n)), std::memory_order_relaxed);
        level_right_.store(static_cast<float>(std::sqrt(sum_r / n)), std::memory_order_relaxed);
    }
}
