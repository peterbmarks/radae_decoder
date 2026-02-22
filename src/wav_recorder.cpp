#include "wav_recorder.h"

#include <cstdlib>
#include <cstring>

/* ── little-endian write helpers ─────────────────────────────────────── */

static void write_u16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
static void write_u32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }

/* ── WAV header (44 bytes) ───────────────────────────────────────────── */

void WavRecorder::write_placeholder_header()
{
    /* Write a complete header with zeroed size fields; patched in close(). */
    uint16_t channels    = static_cast<uint16_t>(channels_);
    uint32_t sample_rate = static_cast<uint32_t>(sample_rate_);
    uint16_t bits        = 16;
    uint16_t block_align = static_cast<uint16_t>(channels * (bits / 8));
    uint32_t byte_rate   = sample_rate * block_align;
    uint32_t zero        = 0;

    std::fwrite("RIFF", 1, 4, file_); write_u32(file_, zero);  /* file size - 8: patched later */
    std::fwrite("WAVE", 1, 4, file_);
    std::fwrite("fmt ", 1, 4, file_); write_u32(file_, 16);
    write_u16(file_, 1);              /* PCM */
    write_u16(file_, channels);
    write_u32(file_, sample_rate);
    write_u32(file_, byte_rate);
    write_u16(file_, block_align);
    write_u16(file_, bits);
    std::fwrite("data", 1, 4, file_); write_u32(file_, zero);  /* data size: patched later */
}

/* ── public API ──────────────────────────────────────────────────────── */

bool WavRecorder::open(const std::string& path, int sample_rate, int channels)
{
    close();

    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) return false;

    sample_rate_ = sample_rate;
    channels_    = channels;
    data_bytes_  = 0;

    write_placeholder_header();
    return true;
}

bool WavRecorder::is_open() const
{
    /* No lock needed — file_ is set under mutex and only cleared in close(). */
    return file_ != nullptr;
}

void WavRecorder::write(const int16_t* samples, int count)
{
    if (!samples || count <= 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;

    size_t written = std::fwrite(samples, sizeof(int16_t),
                                 static_cast<size_t>(count), file_);
    data_bytes_ += static_cast<uint32_t>(written) * sizeof(int16_t);
}

void WavRecorder::close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;

    /* Patch RIFF chunk size at offset 4 */
    uint32_t riff_size = 36 + data_bytes_;
    std::fseek(file_, 4, SEEK_SET);
    write_u32(file_, riff_size);

    /* Patch data chunk size at offset 40 */
    std::fseek(file_, 40, SEEK_SET);
    write_u32(file_, data_bytes_);

    std::fclose(file_);
    file_       = nullptr;
    data_bytes_ = 0;
}
