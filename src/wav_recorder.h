#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

/* ── WavRecorder ───────────────────────────────────────────────────────────
 *
 *  Thread-safe WAV file writer (PCM S16, configurable rate/channels).
 *  write() may be called from any thread while is_open() is true.
 *  close() finalises the WAV header; subsequent write() calls are no-ops.
 * ──────────────────────────────────────────────────────────────────────── */

class WavRecorder {
public:
    WavRecorder()  = default;
    ~WavRecorder() { close(); }

    /* Open a new file for writing.  Deletes any existing file at that path
     * before creating the new one.  Returns false on failure. */
    bool open(const std::string& path, int sample_rate = 8000, int channels = 1);

    /* Append S16 interleaved samples.  Thread-safe; no-op if not open. */
    void write(const int16_t* samples, int count);

    /* Finalise the WAV header and close the file.  Thread-safe. */
    void close();

    bool is_open() const;

private:
    FILE*      file_        = nullptr;
    std::mutex mutex_;
    uint32_t   data_bytes_  = 0;
    int        sample_rate_ = 8000;
    int        channels_    = 1;

    void write_placeholder_header();
};
