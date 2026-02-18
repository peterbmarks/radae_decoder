#include "audio_stream.h"

#include <alsa/asoundlib.h>
#include <string>
#include <vector>
#include <cstdio>

/* ── global init / terminate (ALSA needs no global init) ─────────────────── */

void audio_init()      {}
void audio_terminate() {}

/* ── device enumeration ──────────────────────────────────────────────────── */

static std::vector<AudioDevice> enumerate_alsa(bool capture)
{
    std::vector<AudioDevice> devices;
    fprintf(stderr, "Enumerating ALSA devices\n");
    
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0)
        return devices;

    for (void** hint = hints; *hint; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* desc = snd_device_name_get_hint(*hint, "DESC");
        char* ioid = snd_device_name_get_hint(*hint, "IOID");

        /* ioid: "Input", "Output", or nullptr (both directions) */
        bool ok = !ioid ||
                  ( capture && std::string(ioid) == "Input")  ||
                  (!capture && std::string(ioid) == "Output");

        if (ok && name) {
            AudioDevice ad;
            ad.hw_id = name;
            if (desc) {
                /* ALSA descriptions often have a newline; use just the first line */
                std::string d(desc);
                auto nl = d.find('\n');
                ad.name = (nl != std::string::npos) ? d.substr(0, nl) : d;
            } else {
                ad.name = name;
            }
            devices.push_back(std::move(ad));
        }

        free(name);
        free(desc);
        free(ioid);
    }

    snd_device_name_free_hint(hints);
    return devices;
}

std::vector<AudioDevice> audio_enumerate_capture_devices()
{
    return enumerate_alsa(true);
}

std::vector<AudioDevice> audio_enumerate_playback_devices()
{
    return enumerate_alsa(false);
}

/* ── AudioStream implementation ─────────────────────────────────────────── */

struct AudioStream::Impl {
    snd_pcm_t* pcm      = nullptr;
    int        channels = 1;
};

AudioStream::AudioStream()  = default;
AudioStream::~AudioStream() { close(); }

bool AudioStream::open(const std::string& device_id, bool is_input,
                       int channels, unsigned int sample_rate,
                       unsigned long frames_per_buffer)
{
    fprintf(stderr, "ALSA open\n");

    close();

    const char* dev = device_id.empty() ? "default" : device_id.c_str();
    snd_pcm_stream_t dir = is_input ? SND_PCM_STREAM_CAPTURE
                                    : SND_PCM_STREAM_PLAYBACK;

    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, dev, dir, 0) < 0) {
        fprintf(stderr, "ALSA: snd_pcm_open failed for '%s'\n", dev);
        return false;
    }

    /* Convert frames_per_buffer to microseconds for the latency hint */
    unsigned int latency_us = static_cast<unsigned int>(
        (unsigned long long)frames_per_buffer * 1000000ULL / sample_rate);

    int err = snd_pcm_set_params(pcm,
                                 SND_PCM_FORMAT_S16_LE,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 static_cast<unsigned int>(channels),
                                 sample_rate,
                                 1,           /* allow software resampling */
                                 latency_us);
    if (err < 0) {
        fprintf(stderr, "ALSA: snd_pcm_set_params failed: %s\n",
                snd_strerror(err));
        snd_pcm_close(pcm);
        return false;
    }

    impl_           = new Impl;
    impl_->pcm      = pcm;
    impl_->channels = channels;
    return true;
}

void AudioStream::close()
{
    if (!impl_) return;
    if (impl_->pcm) {
        snd_pcm_drain(impl_->pcm);
        snd_pcm_close(impl_->pcm);
    }
    delete impl_;
    impl_ = nullptr;
}

void AudioStream::stop()
{
    if (impl_ && impl_->pcm)
        snd_pcm_drop(impl_->pcm);
}

void AudioStream::start()
{
    if (impl_ && impl_->pcm)
        snd_pcm_prepare(impl_->pcm);
}

AudioError AudioStream::read(void* buffer, unsigned long frames)
{
    if (!impl_ || !impl_->pcm) return AUDIO_ERROR;

    snd_pcm_sframes_t n = snd_pcm_readi(impl_->pcm, buffer, frames);
    if (n == -EPIPE) {
        /* overrun – recover and signal the caller */
        snd_pcm_prepare(impl_->pcm);
        return AUDIO_OVERFLOW;
    }
    if (n < 0) {
        snd_pcm_recover(impl_->pcm, static_cast<int>(n), 0);
        return AUDIO_ERROR;
    }
    return AUDIO_OK;
}

AudioError AudioStream::write(const void* buffer, unsigned long frames)
{
    if (!impl_ || !impl_->pcm) return AUDIO_ERROR;

    snd_pcm_sframes_t n = snd_pcm_writei(impl_->pcm, buffer, frames);
    if (n == -EPIPE) {
        /* underrun – recover */
        snd_pcm_prepare(impl_->pcm);
        return AUDIO_ERROR;
    }
    if (n < 0) {
        snd_pcm_recover(impl_->pcm, static_cast<int>(n), 0);
        return AUDIO_ERROR;
    }
    return AUDIO_OK;
}
