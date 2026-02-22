#pragma once

#include <string>
#include <vector>

/* ── public types ───────────────────────────────────────────────────────── */

struct AudioDevice {
    std::string name;      // human-readable  e.g. "Built-in Microphone"
    std::string hw_id;     // backend-specific device identifier
};

/* ── error codes ────────────────────────────────────────────────────────── */

enum AudioError {
    AUDIO_OK       = 0,
    AUDIO_OVERFLOW = 1,    // input overflow (non-fatal)
    AUDIO_ERROR    = -1,
};

/* ── global init / terminate ────────────────────────────────────────────── */

void audio_init();
void audio_terminate();

/* ── device enumeration ─────────────────────────────────────────────────── */

std::vector<AudioDevice> audio_enumerate_capture_devices();
std::vector<AudioDevice> audio_enumerate_playback_devices();

/* ── AudioStream ────────────────────────────────────────────────────────── */

class AudioStream {
public:
    AudioStream();
    ~AudioStream();

    AudioStream(const AudioStream&)            = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    /* Open a stream for capture (is_input=true) or playback (is_input=false).
       device_id is a string from AudioDevice::hw_id.
       Returns true on success.  The stream is started immediately. */
    bool open(const std::string& device_id, bool is_input,
              int channels, unsigned int sample_rate,
              unsigned long frames_per_buffer);

    void close();

    /* stop & restart an already-opened stream (discards buffered data) */
    void stop();
    void start();

    /* drain: block until all pending playback data has been played out */
    void drain();

    /* Blocking read/write of S16 mono/stereo interleaved samples.
       Returns AUDIO_OK, AUDIO_OVERFLOW (read only), or AUDIO_ERROR. */
    AudioError read(void* buffer, unsigned long frames);
    AudioError write(const void* buffer, unsigned long frames);

    bool is_open() const { return impl_ != nullptr; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
