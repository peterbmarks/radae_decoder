# Audio Level Meter

A real-time stereo audio level meter for Linux with GTK3 UI and ALSA capture backend.

![Audio Level Meter](https://img.shields.io/badge/Platform-Linux-blue) ![GTK3](https://img.shields.io/badge/GUI-GTK3-green) ![ALSA](https://img.shields.io/badge/Audio-ALSA-orange)

## Features

- **Device enumeration** — Automatically discovers all ALSA capture devices (microphones, line-in, loopback, etc.)
- **Stereo/mono support** — Handles stereo natively; duplicates mono across L+R channels
- **Professional metering** — Calibrated dB scale (−60 to 0 dB) with color-coded gradient:
  - Deep green (low levels)
  - Bright green → yellow-green → yellow (mid levels)
  - Amber → red (approaching 0 dB)
- **Peak hold** — White peak indicator holds for ~1.5 seconds, then decays smoothly
- **Low latency** — 512-frame periods (~11 ms @ 44.1 kHz) for responsive visual feedback
- **Clean UI** — Minimal GTK3 interface with device dropdown, start/stop toggle, and refresh button

## Screenshots

```
┌─────────────────────────────────┐
│ [Device Selector ▼]  [↻]        │
│ [   Start   ]   (green button)  │
│                                 │
│  ┌─┐ ┌──────┐ ┌─┐              │
│  │L│ │  0   │ │R│              │
│  │ │ │ -6   │ │ │              │
│  │█│ │-12   │ │█│  ← gradient  │
│  │█│ │-18   │ │█│    bars      │
│  │█│ │-24   │ │█│              │
│  │█│ │-30   │ │ │              │
│  │ │ │-36   │ │ │              │
│  │ │ │-42   │ │ │              │
│  │ │ │-48   │ │ │              │
│  │ │ │-54   │ │ │              │
│  │ │ │-60   │ │ │              │
│  └─┘ └──────┘ └─┘              │
│                                 │
│  Capturing (stereo)…            │
└─────────────────────────────────┘
```

## Requirements

### Runtime
- Linux (tested on Ubuntu 24.04 / Linux Mint)
- GTK 3.24+
- ALSA runtime library (`libasound2`)
- X11 or Wayland display server

### Build-time
- CMake 3.10+
- C++17 compiler (GCC 7+, Clang 5+)
- Development headers:
  - `libgtk-3-dev` (GTK3 headers + pkg-config)
  - `libasound2-dev` (ALSA headers)
  - `libcairo2-dev` (usually pulled in by GTK3)

### Install dependencies (Debian/Ubuntu)
```bash
sudo apt-get install build-essential cmake \
  libgtk-3-dev libasound2-dev pkg-config
```

## Build Instructions

```bash
# Clone or navigate to the project directory
cd SimpleDecoder

# Configure with CMake
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..

# Compile (parallel build with all cores)
make -j$(nproc)

# Binary is now at: build/audio_level_meter
```

### Environment quirks

On some systems, pkg-config can't find `.pc` files in `/usr/lib/x86_64-linux-gnu/pkgconfig`. The CMakeLists.txt handles this automatically by setting `PKG_CONFIG_PATH`, but if you encounter issues, manually export:

```bash
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig
cmake ..
```

## Usage

```bash
# From the build directory
./audio_level_meter

# Or install system-wide (optional)
sudo make install
audio_level_meter
```

### First run

1. **Device dropdown** — Lists all ALSA capture devices. Select your microphone or input source.
2. **Start button** — Click to begin capturing audio. The button turns red and changes to "Stop".
3. **Meter display** — Watch the stereo bars respond to audio in real-time.
4. **Refresh button** (↻) — Re-scan for devices if you plug in new hardware.

### Permissions

If you see "Failed to open device", ensure your user is in the `audio` group:

```bash
sudo usermod -a -G audio $USER
# Log out and back in for the change to take effect
```

## Architecture

### Code structure

```
SimpleDecoder/
├── CMakeLists.txt           # Build configuration
├── README.md                # This file
└── src/
    ├── main.cpp             # GTK application, UI setup, event handlers
    ├── audio_input.h/cpp    # ALSA device enumeration & capture thread
    └── meter_widget.h/cpp   # Cairo-based stereo bar meter widget
```

### Component overview

| Module | Responsibility |
|--------|---------------|
| **audio_input** | Wraps ALSA PCM API; enumerates capture devices; spawns a background thread that continuously reads audio frames and computes per-channel RMS levels (stored as `std::atomic<float>` for lock-free access) |
| **meter_widget** | Custom `GtkDrawingArea` widget; redraws at ~30 fps using Cairo; converts linear RMS → logarithmic dB position; applies green→red gradient; tracks peak-hold with decay logic |
| **main** | GTK application shell; connects signals; manages device combo box; toggles capture on/off; updates meter via `g_timeout_add` timer |

### Audio pipeline

```
ALSA PCM device (hw:X,Y)
  ↓ snd_pcm_readi() — blocking read of 512 frames (S16_LE, interleaved)
  ↓ Per-channel RMS calculation (√(Σ(sample²) / N))
  ↓ std::atomic<float> level_left, level_right
  ↓ [lock-free handoff]
  ↓ GLib timer callback (33 ms / ~30 Hz)
  ↓ meter_widget_update() — peak-hold logic, queue GTK redraw
  ↓ Cairo draw callback — dB scale, gradient fill, peak line
```

### Signal flow

1. User selects device → `on_combo_changed()` → `start_capture(idx)`
2. `AudioInput::open(hw_id)` → configure ALSA (44.1 kHz, S16_LE, stereo/mono)
3. `AudioInput::start()` → spawn `capture_loop()` thread
4. Thread runs tight loop: `snd_pcm_readi()` → compute RMS → store atomics
5. Main thread: `g_timeout_add(33 ms)` → `on_meter_tick()` → read atomics → `meter_widget_update()`
6. GTK: `gtk_widget_queue_draw()` → `on_draw()` → Cairo rendering

## Troubleshooting

### "Package 'gtk+-3.0' not found"
Your pkg-config search path is missing the multiarch directory. The CMakeLists.txt should handle this, but you can manually fix:
```bash
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig
```

### "alsa/alsa.h: No such file or directory"
The ALSA headers use `alsa/asoundlib.h` as the main include (not `alsa/alsa.h`). This project already uses the correct header. If you see this error, ensure `libasound2-dev` is installed.

### "Failed to open device"
- Check permissions: `groups` should list `audio`. If not: `sudo usermod -a -G audio $USER` and re-login.
- Verify the device exists: `arecord -l` lists capture devices.
- Try a different device from the dropdown.

### Window doesn't appear
Ensure you're running on a display server (X11/Wayland) with `$DISPLAY` set. Test GTK is working: `gtk3-demo`.

### Choppy or delayed meter response
The default buffer/period sizes target ~11 ms latency. If your system can't sustain this:
- Edit [audio_input.cpp:98-99](src/audio_input.cpp#L98-L99) and increase `period` to 1024 or 2048.
- Rebuild: `make -j$(nproc)`

## Configuration

Most settings are baked into the source. To customize:

| Setting | File | Line(s) |
|---------|------|---------|
| Audio format | [audio_input.cpp](src/audio_input.cpp) | 76-94 (S16_LE, 44.1 kHz, stereo) |
| Period/buffer size | [audio_input.cpp](src/audio_input.cpp) | 98-101 (512 / 2048 frames) |
| Meter update rate | [main.cpp](src/main.cpp) | 78 (`g_timeout_add(33 ms)` ≈ 30 Hz) |
| dB range | [meter_widget.cpp](src/meter_widget.cpp) | 15-16 (−60 to 0 dB) |
| Peak hold time | [meter_widget.cpp](src/meter_widget.cpp) | 13 (45 frames @ 30 Hz ≈ 1.5 s) |
| Color gradient stops | [meter_widget.cpp](src/meter_widget.cpp) | 82-89 (green → red) |
| Window size | [main.cpp](src/main.cpp) | 169 (300×480 px) |

After changes, rebuild with `make -j$(nproc)`.

## Technical details

### ALSA configuration
- **Access mode**: `SND_PCM_ACCESS_RW_INTERLEAVED` (simplest API)
- **Format**: `SND_PCM_FORMAT_S16_LE` (16-bit signed little-endian)
- **Sample rate**: 44100 Hz (CD quality)
- **Channels**: 2 (stereo preferred), falls back to 1 (mono duplicated)
- **Periods**: 512 frames (~11.6 ms @ 44.1 kHz)
- **Buffer**: 2048 frames (~46.4 ms total latency)

### RMS to dB conversion
```cpp
float rms = sqrt(sum_of_squares / num_samples);  // linear amplitude (0..1)
float db = 20 * log10(rms);                      // convert to dB
float pos = (db - DB_MIN) / (DB_MAX - DB_MIN);   // map −60..0 dB → 0..1
```

### Peak-hold algorithm
```cpp
if (current_level >= peak) {
    peak = current_level;
    hold_timer = PEAK_HOLD;  // 45 frames
} else {
    if (hold_timer > 0) {
        hold_timer--;
    } else {
        peak *= PEAK_DECAY;  // 0.925 per frame → ~10% decay/sec
    }
}
```

### Thread safety
- Capture thread writes to `std::atomic<float>` (relaxed ordering)
- GTK main thread reads atomics → no mutex needed, no blocking

## Roadmap

Potential future enhancements:
- [ ] VU meter mode (slower ballistics, −20 to +3 VU scale)
- [ ] Peak clip indicator (stays red for N seconds when 0 dB hit)
- [ ] Configurable sample rate and buffer sizes (via UI or config file)
- [ ] Spectrum analyzer view (FFT-based frequency bands)
- [ ] Export functionality (CSV logging of peak/RMS over time)
- [ ] PulseAudio / PipeWire backend option
- [ ] Dark theme toggle

## License

This project is provided as-is for educational and personal use. Modify and distribute freely.

## Credits

- Built with GTK 3 ([gtk.org](https://www.gtk.org/))
- Audio capture via ALSA ([alsa-project.org](https://www.alsa-project.org/))
- Graphics rendering with Cairo ([cairographics.org](https://www.cairographics.org/))

---

**Author**: SimpleDecoder project
**Created**: 2026
**Platform**: Linux (Ubuntu 24.04 / Linux Mint)
