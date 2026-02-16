#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <portaudio.h>

/* ── public types ───────────────────────────────────────────────────────── */

struct AudioDevice {
    std::string name;       // human-readable  e.g. "Built-in Audio — Microphone"
    int         device_idx; // PortAudio device index
};

/* ── AudioInput ─────────────────────────────────────────────────────────── */

class AudioInput {
public:
    AudioInput();
    ~AudioInput();

    /* device list ---------------------------------------------------------- */
    static std::vector<AudioDevice> enumerate_devices();           // capture devices
    static std::vector<AudioDevice> enumerate_playback_devices();  // playback devices

    /* lifecycle ------------------------------------------------------------ */
    bool open(int device_idx);             // configure + prepare
    void close();                          // stop (if running) + close handle
    void start();                          // launch capture thread
    void stop();                           // join capture thread

    /* queries -------------------------------------------------------------- */
    bool  is_running()        const { return running_.load(std::memory_order_relaxed); }
    int   channels()          const { return channels_; }
    float get_level_left()    const { return level_left_.load(std::memory_order_relaxed); }
    float get_level_right()   const { return level_right_.load(std::memory_order_relaxed); }

private:
    void capture_loop();

    PaStream*          stream_   = nullptr;
    int                channels_ = 0;
    std::thread        thread_;
    std::atomic<bool>  running_  {false};
    std::atomic<float> level_left_  {0.0f};
    std::atomic<float> level_right_ {0.0f};
};
