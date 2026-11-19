#include "audio_stream.h"

#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <thread>

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

/* ── strict playback via the asynchronous PulseAudio API ────────────────────
 *
 *  pa_simple cannot tell us where the server actually routed a stream.  Under
 *  PipeWire, if the requested sink cannot start (e.g. a USB sound card that
 *  runs out of isochronous bandwidth) WirePlumber silently relinks the stream
 *  to the default output — so TX audio ends up in the speakers.  A "strict"
 *  playback stream asks the server not to move it (PA_STREAM_DONT_MOVE) and
 *  reports failure when it is moved anyway, dies, or stops accepting audio.
 * ──────────────────────────────────────────────────────────────────────── */

namespace {

struct StrictPlayback {
    pa_threaded_mainloop* ml     = nullptr;
    pa_context*           ctx    = nullptr;
    pa_stream*            stream = nullptr;
    std::string           device;
    std::atomic<bool>     failed{false};
};

/* how long write() may make no progress before the device is declared dead */
constexpr auto SP_STALL_LIMIT = std::chrono::seconds(3);
constexpr auto SP_POLL        = std::chrono::milliseconds(5);

void sp_fail(StrictPlayback* sp, const char* why)
{
    if (!sp->failed.exchange(true))
        fprintf(stderr, "PulseAudio: playback to %s failed: %s\n",
                sp->device.c_str(), why);
}

void sp_context_state_cb(pa_context* /*c*/, void* ud)
{
    pa_threaded_mainloop_signal(static_cast<StrictPlayback*>(ud)->ml, 0);
}

void sp_stream_state_cb(pa_stream* s, void* ud)
{
    auto* sp = static_cast<StrictPlayback*>(ud);
    pa_stream_state_t st = pa_stream_get_state(s);
    if (st == PA_STREAM_FAILED)     sp_fail(sp, "stream failed");
    if (st == PA_STREAM_TERMINATED) sp_fail(sp, "stream terminated");
    pa_threaded_mainloop_signal(sp->ml, 0);
}

void sp_stream_moved_cb(pa_stream* s, void* ud)
{
    auto* sp = static_cast<StrictPlayback*>(ud);
    const char* now = pa_stream_get_device_name(s);
    if (!now || sp->device != now) {
        fprintf(stderr, "PulseAudio: playback stream moved to %s\n", now ? now : "(none)");
        sp_fail(sp, "stream was moved to another device");
    }
    pa_threaded_mainloop_signal(sp->ml, 0);
}

void sp_signal_cb(pa_stream* /*s*/, size_t /*n*/, void* ud)
{
    pa_threaded_mainloop_signal(static_cast<StrictPlayback*>(ud)->ml, 0);
}

void sp_success_cb(pa_stream* /*s*/, int /*ok*/, void* ud)
{
    pa_threaded_mainloop_signal(static_cast<StrictPlayback*>(ud)->ml, 0);
}

/* Wait (mainloop locked) for op to complete; give up after timeout or on failure. */
bool sp_wait_op(StrictPlayback* sp, pa_operation* op, std::chrono::milliseconds timeout)
{
    if (!op) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        if (sp->failed || std::chrono::steady_clock::now() >= deadline) {
            pa_operation_cancel(op);
            pa_operation_unref(op);
            return false;
        }
        pa_threaded_mainloop_unlock(sp->ml);
        std::this_thread::sleep_for(SP_POLL);
        pa_threaded_mainloop_lock(sp->ml);
    }
    pa_operation_unref(op);
    return true;
}

void sp_destroy(StrictPlayback* sp)
{
    if (!sp) return;
    if (sp->ml) pa_threaded_mainloop_lock(sp->ml);
    if (sp->stream) {
        pa_stream_set_state_callback(sp->stream, nullptr, nullptr);
        pa_stream_set_moved_callback(sp->stream, nullptr, nullptr);
        pa_stream_set_write_callback(sp->stream, nullptr, nullptr);
        pa_stream_disconnect(sp->stream);
        pa_stream_unref(sp->stream);
    }
    if (sp->ctx) {
        pa_context_set_state_callback(sp->ctx, nullptr, nullptr);
        pa_context_disconnect(sp->ctx);
        pa_context_unref(sp->ctx);
    }
    if (sp->ml) {
        pa_threaded_mainloop_unlock(sp->ml);
        pa_threaded_mainloop_stop(sp->ml);
        pa_threaded_mainloop_free(sp->ml);
    }
    delete sp;
}

StrictPlayback* sp_open(const std::string& device, const pa_sample_spec& ss)
{
    auto* sp = new StrictPlayback;
    sp->device = device;

    sp->ml = pa_threaded_mainloop_new();
    if (!sp->ml) { sp_destroy(sp); return nullptr; }
    sp->ctx = pa_context_new(pa_threaded_mainloop_get_api(sp->ml), "radae");
    if (!sp->ctx) { sp_destroy(sp); return nullptr; }
    pa_context_set_state_callback(sp->ctx, sp_context_state_cb, sp);
    if (pa_threaded_mainloop_start(sp->ml) < 0) { sp_destroy(sp); return nullptr; }

    pa_threaded_mainloop_lock(sp->ml);

    bool ok = pa_context_connect(sp->ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0;
    while (ok) {
        pa_context_state_t st = pa_context_get_state(sp->ctx);
        if (st == PA_CONTEXT_READY) break;
        if (!PA_CONTEXT_IS_GOOD(st)) ok = false;
        else pa_threaded_mainloop_wait(sp->ml);
    }

    if (ok) {
        sp->stream = pa_stream_new(sp->ctx, "playback", &ss, nullptr);
        ok = sp->stream != nullptr;
    }
    if (ok) {
        pa_stream_set_state_callback(sp->stream, sp_stream_state_cb, sp);
        pa_stream_set_moved_callback(sp->stream, sp_stream_moved_cb, sp);
        pa_stream_set_write_callback(sp->stream, sp_signal_cb, sp);
        /* same flags pa_simple uses, plus: never move this stream */
        auto flags = static_cast<pa_stream_flags_t>(
            PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY |
            PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_DONT_MOVE);
        ok = pa_stream_connect_playback(sp->stream,
                                        device.empty() ? nullptr : device.c_str(),
                                        nullptr, flags, nullptr, nullptr) >= 0;
    }
    while (ok) {
        pa_stream_state_t st = pa_stream_get_state(sp->stream);
        if (st == PA_STREAM_READY) break;
        if (!PA_STREAM_IS_GOOD(st)) ok = false;
        else pa_threaded_mainloop_wait(sp->ml);
    }
    if (ok && !device.empty()) {
        const char* actual = pa_stream_get_device_name(sp->stream);
        if (!actual || device != actual) {
            fprintf(stderr, "PulseAudio: asked for %s but got %s\n",
                    device.c_str(), actual ? actual : "(none)");
            ok = false;
        }
    }
    if (ok && sp->failed) ok = false;

    pa_threaded_mainloop_unlock(sp->ml);

    if (!ok) {
        fprintf(stderr, "PulseAudio: could not open playback device %s\n", device.c_str());
        sp_destroy(sp);
        return nullptr;
    }
    return sp;
}

AudioError sp_write(StrictPlayback* sp, const void* buffer, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(buffer);

    pa_threaded_mainloop_lock(sp->ml);
    auto last_progress = std::chrono::steady_clock::now();
    while (bytes > 0) {
        if (sp->failed || !PA_STREAM_IS_GOOD(pa_stream_get_state(sp->stream))) {
            pa_threaded_mainloop_unlock(sp->ml);
            return AUDIO_ERROR;
        }
        size_t n = pa_stream_writable_size(sp->stream);
        if (n == static_cast<size_t>(-1)) {
            sp_fail(sp, "stream not writable");
            continue;
        }
        if (n == 0) {
            if (std::chrono::steady_clock::now() - last_progress > SP_STALL_LIMIT) {
                sp_fail(sp, "device stopped accepting audio");
                continue;
            }
            pa_threaded_mainloop_unlock(sp->ml);
            std::this_thread::sleep_for(SP_POLL);
            pa_threaded_mainloop_lock(sp->ml);
            continue;
        }
        if (n > bytes) n = bytes;
        if (pa_stream_write(sp->stream, p, n, nullptr, 0, PA_SEEK_RELATIVE) < 0) {
            sp_fail(sp, "write failed");
            continue;
        }
        p     += n;
        bytes -= n;
        last_progress = std::chrono::steady_clock::now();
    }
    pa_threaded_mainloop_unlock(sp->ml);
    return AUDIO_OK;
}

void sp_drain(StrictPlayback* sp, std::chrono::milliseconds timeout)
{
    pa_threaded_mainloop_lock(sp->ml);
    if (!sp->failed)
        sp_wait_op(sp, pa_stream_drain(sp->stream, sp_success_cb, sp), timeout);
    pa_threaded_mainloop_unlock(sp->ml);
}

void sp_flush(StrictPlayback* sp)
{
    pa_threaded_mainloop_lock(sp->ml);
    if (!sp->failed)
        sp_wait_op(sp, pa_stream_flush(sp->stream, sp_success_cb, sp),
                   std::chrono::milliseconds(1000));
    pa_threaded_mainloop_unlock(sp->ml);
}

} // namespace

/* ── AudioStream implementation ─────────────────────────────────────────────
 *  Capture and ordinary playback use pa_simple; playback with
 *  set_no_fallback(true) uses StrictPlayback above. */

struct AudioStream::Impl {
    pa_simple*      simple      = nullptr;
    StrictPlayback* strict      = nullptr;
    unsigned int    sample_rate = 0;
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

    if (!is_input && no_fallback_) {
        StrictPlayback* sp = sp_open(device_id, ss);
        if (!sp) return false;
        impl_ = new Impl;
        impl_->strict      = sp;
        impl_->sample_rate = sample_rate;
        return true;
    }

    const char* dev = device_id.empty() ? nullptr : device_id.c_str();
    /* For recording, override the default fragsize so PulseAudio delivers
       data in small chunks matching frames_per_buffer.  The default is
       often 1-2 seconds, causing pa_simple_read() to block that long and
       producing visible gaps in the spectrum display.
       Leave playback with the server default (nullptr) to avoid underruns. */
    pa_buffer_attr attr{};
    attr.tlength   = static_cast<uint32_t>(-1);
    attr.prebuf    = static_cast<uint32_t>(-1);
    attr.minreq    = static_cast<uint32_t>(-1);
    attr.fragsize  = static_cast<uint32_t>(frames_per_buffer)
                   * static_cast<uint32_t>(channels)
                   * static_cast<uint32_t>(pa_sample_size(&ss));
    /* Cap the recording ring buffer to 4 fragments so stale audio cannot
       accumulate and delay the spectrum display.  PulseAudio will drop
       overflows rather than growing the buffer indefinitely. */
    //attr.maxlength = attr.fragsize * 4; // does not decode
    attr.maxlength = static_cast<uint32_t>(-1);

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
    impl_->simple      = s;
    impl_->sample_rate = sample_rate;
    return true;
}

void AudioStream::close()
{
    if (!impl_) return;
    if (impl_->simple) pa_simple_free(impl_->simple);
    if (impl_->strict) sp_destroy(impl_->strict);
    delete impl_;
    impl_ = nullptr;
}

void AudioStream::stop()
{
    if (impl_ && impl_->strict) { sp_flush(impl_->strict); return; }
    if (impl_ && impl_->simple)
        pa_simple_flush(impl_->simple, nullptr);
}

void AudioStream::start()
{
    /* pa_simple has no explicit start; streaming resumes on next read/write */
}

void AudioStream::drain()
{
    if (impl_ && impl_->strict) {
        sp_drain(impl_->strict, std::chrono::milliseconds(5000));
        return;
    }
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
    if (impl_ && impl_->strict)
        return sp_write(impl_->strict, buffer, frames * 2);   /* S16 mono */

    if (!impl_ || !impl_->simple) return AUDIO_ERROR;

    size_t bytes = frames * 2;   /* S16 mono = 2 bytes per frame */
    int error = 0;
    if (pa_simple_write(impl_->simple, buffer, bytes, &error) < 0)
        return AUDIO_ERROR;
    return AUDIO_OK;
}

long AudioStream::read_available()
{
    if (!impl_ || !impl_->simple) return -1;

    /* For a record stream this is the audio captured but not yet read
       (plus a little device latency). */
    int error = 0;
    pa_usec_t usec = pa_simple_get_latency(impl_->simple, &error);
    if (usec == static_cast<pa_usec_t>(-1)) return -1;
    return static_cast<long>(usec * impl_->sample_rate / 1000000ULL);
}
