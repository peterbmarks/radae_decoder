#include "audio_stream.h"

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <string>
#include <vector>
#include <cstring>

/* ── global init / terminate (PulseAudio needs no global init) ──────────── */

void audio_init()      {}
void audio_terminate() {}

/* ── device enumeration via PulseAudio mainloop API ─────────────────────── */

struct EnumCtx {
    std::vector<AudioDevice>* devices;
    bool done;
};

static void source_info_cb(pa_context* /*c*/, const pa_source_info* i,
                            int eol, void* userdata)
{
    auto* ctx = static_cast<EnumCtx*>(userdata);
    if (eol > 0) { ctx->done = true; return; }
    if (!i) return;

    AudioDevice ad;
    ad.name  = i->description ? i->description : i->name;
    ad.hw_id = i->name;
    ctx->devices->push_back(std::move(ad));
}

static void sink_info_cb(pa_context* /*c*/, const pa_sink_info* i,
                          int eol, void* userdata)
{
    auto* ctx = static_cast<EnumCtx*>(userdata);
    if (eol > 0) { ctx->done = true; return; }
    if (!i) return;

    AudioDevice ad;
    ad.name  = i->description ? i->description : i->name;
    ad.hw_id = i->name;
    ctx->devices->push_back(std::move(ad));
}

static void context_state_cb(pa_context* c, void* userdata)
{
    auto* ready = static_cast<bool*>(userdata);
    pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED ||
        state == PA_CONTEXT_TERMINATED)
        *ready = true;
}

static std::vector<AudioDevice> enumerate_pulse(bool capture)
{
    fprintf(stderr, "Enumerating PaulseAudio devices\n");
    
    std::vector<AudioDevice> devices;

    pa_mainloop* ml = pa_mainloop_new();
    if (!ml) return devices;

    pa_mainloop_api* api = pa_mainloop_get_api(ml);
    pa_context* ctx = pa_context_new(api, "radae-enum");
    if (!ctx) { pa_mainloop_free(ml); return devices; }

    bool ready = false;
    pa_context_set_state_callback(ctx, context_state_cb, &ready);
    pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    /* wait for connection */
    while (!ready)
        pa_mainloop_iterate(ml, 1, nullptr);

    if (pa_context_get_state(ctx) != PA_CONTEXT_READY) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return devices;
    }

    EnumCtx ectx{&devices, false};

    if (capture)
        pa_context_get_source_info_list(ctx, source_info_cb, &ectx);
    else
        pa_context_get_sink_info_list(ctx, sink_info_cb, &ectx);

    while (!ectx.done)
        pa_mainloop_iterate(ml, 1, nullptr);

    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    return devices;
}

std::vector<AudioDevice> audio_enumerate_capture_devices()
{
    return enumerate_pulse(true);
}

std::vector<AudioDevice> audio_enumerate_playback_devices()
{
    return enumerate_pulse(false);
}

/* ── AudioStream implementation via pa_simple ───────────────────────────── */

struct AudioStream::Impl {
    pa_simple* simple = nullptr;
};

AudioStream::AudioStream()  = default;
AudioStream::~AudioStream() { close(); }

bool AudioStream::open(const std::string& device_id, bool is_input,
                       int channels, unsigned int sample_rate,
                       unsigned long frames_per_buffer)
{
    fprintf(stderr, "PulseAudio open\n");

    close();

    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = sample_rate;
    ss.channels = static_cast<uint8_t>(channels);

    const char* dev = device_id.empty() ? nullptr : device_id.c_str();

    /* For recording, override the default fragsize so PulseAudio delivers
       data in small chunks matching frames_per_buffer.  The default is
       often 1-2 seconds, causing pa_simple_read() to block that long and
       producing visible gaps in the spectrum display.
       Leave playback with the server default (nullptr) to avoid underruns. */
    pa_buffer_attr attr{};
    attr.maxlength = static_cast<uint32_t>(-1);
    attr.tlength   = static_cast<uint32_t>(-1);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);
    attr.fragsize  = static_cast<uint32_t>(frames_per_buffer)
                   * static_cast<uint32_t>(channels)
                   * static_cast<uint32_t>(pa_sample_size(&ss));

    int error = 0;
    pa_simple* s = pa_simple_new(
        nullptr,                              /* server */
        "radae",                              /* app name */
        is_input ? PA_STREAM_RECORD : PA_STREAM_PLAYBACK,
        dev,                                  /* device */
        is_input ? "capture" : "playback",    /* stream name */
        &ss,                                  /* sample spec */
        nullptr,                              /* channel map */
        is_input ? &attr : nullptr,           /* buffering attributes */
        &error);

    if (!s) return false;

    impl_ = new Impl;
    impl_->simple = s;
    return true;
}

void AudioStream::close()
{
    if (!impl_) return;
    if (impl_->simple) pa_simple_free(impl_->simple);
    delete impl_;
    impl_ = nullptr;
}

void AudioStream::stop()
{
    if (impl_ && impl_->simple)
        pa_simple_flush(impl_->simple, nullptr);
}

void AudioStream::start()
{
    /* pa_simple has no explicit start; streaming resumes on next read/write */
}

void AudioStream::drain()
{
    if (impl_ && impl_->simple)
        pa_simple_drain(impl_->simple, nullptr);
}

AudioError AudioStream::read(void* buffer, unsigned long frames)
{
    if (!impl_ || !impl_->simple) return AUDIO_ERROR;

    /* pa_simple_read takes byte count */
    size_t bytes = frames * 2;   /* S16 mono = 2 bytes per frame */
    /* Note: for stereo the caller should account for channels in the buffer,
       but pa_simple handles it automatically via the sample spec */
    int error = 0;
    if (pa_simple_read(impl_->simple, buffer, bytes, &error) < 0)
        return AUDIO_ERROR;
    return AUDIO_OK;
}

AudioError AudioStream::write(const void* buffer, unsigned long frames)
{
    if (!impl_ || !impl_->simple) return AUDIO_ERROR;

    size_t bytes = frames * 2;   /* S16 mono = 2 bytes per frame */
    int error = 0;
    if (pa_simple_write(impl_->simple, buffer, bytes, &error) < 0)
        return AUDIO_ERROR;
    return AUDIO_OK;
}
