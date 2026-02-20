#include "audio_stream.h"
#include <portaudio.h>
#include <string>
#include <vector>

/* ── global init / terminate ────────────────────────────────────────────── */

void audio_init()      { Pa_Initialize(); }
void audio_terminate() { Pa_Terminate();  }

/* ── device enumeration ─────────────────────────────────────────────────── */

std::vector<AudioDevice> audio_enumerate_capture_devices()
{
    std::vector<AudioDevice> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;

        AudioDevice ad;
        ad.name  = info->name;
        ad.hw_id = std::to_string(i);
        devices.push_back(std::move(ad));
    }
    return devices;
}

std::vector<AudioDevice> audio_enumerate_playback_devices()
{
    std::vector<AudioDevice> devices;
    int count = Pa_GetDeviceCount();
    for (int i = 0; i < count; i++) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;

        AudioDevice ad;
        ad.name  = info->name;
        ad.hw_id = std::to_string(i);
        devices.push_back(std::move(ad));
    }
    return devices;
}

/* ── AudioStream implementation ─────────────────────────────────────────── */

struct AudioStream::Impl {
    PaStream* stream = nullptr;
};

AudioStream::AudioStream()  = default;
AudioStream::~AudioStream() { close(); }

bool AudioStream::open(const std::string& device_id, bool is_input,
                       int channels, unsigned int sample_rate,
                       unsigned long frames_per_buffer)
{
    fprintf(stderr, "PortAudio open\n");

    close();

    PaDeviceIndex dev = static_cast<PaDeviceIndex>(std::stoi(device_id));
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (!info) return false;

    PaStreamParameters params{};
    params.device                    = dev;
    params.channelCount              = channels;
    params.sampleFormat              = paInt16;
    params.suggestedLatency          = is_input ? info->defaultLowInputLatency
                                                : info->defaultHighOutputLatency;
    params.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(
        &stream,
        is_input  ? &params : nullptr,
        !is_input ? &params : nullptr,
        static_cast<double>(sample_rate),
        frames_per_buffer,
        paClipOff,
        nullptr, nullptr);

    if (err != paNoError) return false;

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        Pa_CloseStream(stream);
        return false;
    }

    impl_ = new Impl;
    impl_->stream = stream;
    return true;
}

void AudioStream::close()
{
    if (!impl_) return;
    Pa_StopStream(impl_->stream);
    Pa_CloseStream(impl_->stream);
    delete impl_;
    impl_ = nullptr;
}

void AudioStream::stop()
{
    if (impl_) Pa_StopStream(impl_->stream);
}

void AudioStream::start()
{
    if (impl_) Pa_StartStream(impl_->stream);
}

AudioError AudioStream::read(void* buffer, unsigned long frames)
{
    if (!impl_) return AUDIO_ERROR;
    PaError err = Pa_ReadStream(impl_->stream, buffer, frames);
    if (err == paNoError)        return AUDIO_OK;
    if (err == paInputOverflowed) return AUDIO_OVERFLOW;
    return AUDIO_ERROR;
}

AudioError AudioStream::write(const void* buffer, unsigned long frames)
{
    if (!impl_) return AUDIO_ERROR;
    PaError err = Pa_WriteStream(impl_->stream, buffer, frames);
    if (err == paNoError) return AUDIO_OK;
    return AUDIO_ERROR;
}
