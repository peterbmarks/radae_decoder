# RADAE_Gui

Originally from: https://github.com/peterbmarks/radae_decoder

Based on code from: https://github.com/drowe67/radae

A real-time RADAE (Radio Autoencoder) encoder and decoder for Linux. In receive (RX) mode it captures RADAE modem audio from a PulseAudio input device, decodes it using a neural OFDM demodulator and FARGAN vocoder, and plays the decoded speech on a PulseAudio output device. In transmit (TX) mode it captures speech from a microphone, extracts LPCNet features, encodes them with the RADE neural encoder, and outputs the OFDM modem signal to a radio. Includes a GTK3 UI with level meters, spectrum/waterfall displays, sync status, SNR, and a TX output level slider.

![Platform](https://img.shields.io/badge/Platform-Linux-blue) ![GTK3](https://img.shields.io/badge/GUI-GTK3-green) ![PulseAudio](https://img.shields.io/badge/Audio-PulseAudio-orange) ![RADAE](https://img.shields.io/badge/Codec-RADAE-purple)


![Screenshot](screenshot.png)

![Settings](settings.png)

[Video demo](https://youtu.be/Q1SExfmMqZ0?si=LSMlgETFaZ1H1Fn5)

Unlike the official FreeDV app, this program uses an experimental C port of the python code and does
not require python to run. It's (currently) a statically linked single binary of just 11MB compared to
600MB. (But, of course, it does far far less).

## Features

### Receive (RX)
- **Real-time RADAE decoding** — Full receive pipeline: Hilbert transform, OFDM demodulation, neural decoder, FARGAN speech synthesis
- **Dual device selection** — Pick any PulseAudio capture device (modem input) and playback device (decoded speech output)
- **Automatic signal acquisition** — Searches for RADAE signal, locks on when found, re-acquires after signal loss
- **Live status display** — Shows sync state, SNR (dB), and frequency offset (Hz) while decoding
- **Open WAV file recording** — Decodes and plays a WAV file recording such as those from the FreeDV app

### Transmit (TX)
- **Real-time RADAE encoding** — Full transmit pipeline: microphone capture, LPCNet feature extraction, neural RADE encoder, OFDM modulation
- **Dual device selection** — Pick any PulseAudio capture device (microphone) and playback device (radio transmit audio)
- **TX output level slider** — Adjustable output level (0–100%) to set the drive level to the radio; saved across sessions
- **End-of-over signalling** — Automatically sends an EOO frame when transmission stops

### Common
- **Input & output level meters** — Calibrated dB scale (-60 to 0 dB) with peak hold
- **Spectrum display** — Shows 4 kHz of audio spectrum. With a RADAE signal you should see energy concentrated in the OFDM band around 1.3 kHz
- **Waterfall display** — Same as the spectrum but with vertical history
- **Sample rate flexibility** — PulseAudio handles sample rate conversion; internally works at 8 kHz modem and 16 kHz speech rates
- **Settings persistence** — Device selections and TX level are saved to `~/.config/radae-decoder.conf`

## How it works

### RX (Decode)
```
PulseAudio Input (8 kHz mono)
  -> Hilbert transform (127-tap FIR) -> complex IQ
  -> RADE receiver (pilot acquisition, OFDM demod, neural decoder)
  -> FARGAN vocoder -> 16 kHz mono speech
  -> PulseAudio Output
```

### TX (Encode)
```
PulseAudio Mic Input (16 kHz mono)
  -> LPCNet feature extraction (36 features per 10 ms frame)
  -> Accumulate 12 feature frames (120 ms)
  -> RADE transmitter (neural encoder, OFDM modulation)
  -> 960 complex IQ samples @ 8 kHz -> take real part
  -> Scale by TX output level
  -> PulseAudio Radio Output
```

The RADAE codec uses a 30-carrier OFDM waveform in ~1.3 kHz bandwidth. Each 120 ms modem frame produces 12 speech frames (10 ms each) via the neural decoder and FARGAN vocoder. Pilot symbols enable automatic frequency and timing synchronization.

## Requirements

### Runtime
- Linux (tested on Ubuntu 24.04 / Linux Mint)
- GTK 3.24+
- PulseAudio runtime libraries (`libpulse0`)
- X11 or Wayland display server

### Build-time
- CMake 3.16+
- C++17 compiler (GCC 7+, Clang 5+)
- C11 compiler
- Internet connection (first build downloads and compiles Opus with FARGAN/LPCNet support)
- Autotools (`autoconf`, `automake`, `libtool`) for building Opus
- Development headers:
  - `libgtk-3-dev`
  - `libpulse-dev`
  - `libcairo2-dev` (usually pulled in by GTK3)

### Install dependencies (Debian/Ubuntu)
```bash
sudo apt-get install build-essential cmake \
  libgtk-3-dev libpulse-dev pkg-config \
  autoconf automake libtool libhamlib++-dev \
  libhamlib-dev libpulse-dev
```

## Build Instructions

```bash
cd radae_decoder

mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -Wno-dev ..

# First build downloads Opus (~175 MB) and compiles everything.
# The NN weight files (rade_enc_data.c, rade_dec_data.c) are ~47 MB
# and take a while to compile.
make -j$(nproc)

# Binary is now at: build/radae_decoder
```

### Environment quirks

On some systems, pkg-config can't find `.pc` files in `/usr/lib/x86_64-linux-gnu/pkgconfig`. The CMakeLists.txt handles this automatically, but if you encounter issues:

```bash
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig
cmake ..
```

## Usage

```bash
./build/radae_decoder
```

### First run (RX)

1. **Input dropdown** -- Select the PulseAudio capture device receiving the RADAE modem signal (e.g. a sound card connected to a radio receiver).
2. **Output dropdown** -- Select the PulseAudio playback device for decoded speech (e.g. speakers or headphones).
3. **Start button** -- Click to begin decoding. The button turns red and changes to "Stop".
4. **Status bar** -- Shows "Searching for signal..." until a RADAE signal is detected, then displays "Synced -- SNR: X dB  Freq: +Y Hz".
5. **Meter display** -- Shows decoded output audio levels in real-time once synced.
6. **Refresh button** -- Re-scan for devices if you plug in new hardware.

The chosen input and output audio device names are saved and loaded on launch. If both are found
decoding will automatically start.

### Transmitting (TX)

1. Toggle the **TX switch** to enable transmit mode. The settings dialog gains TX-specific device selectors.
2. **TX Input** -- Select the microphone capture device.
3. **TX Output** -- Select the PulseAudio playback device connected to the radio transmitter.
4. **TX level slider** -- Adjust the output drive level (right side of window). The setting is saved across sessions.
5. Click **Start** to begin transmitting. The status bar shows "Transmitting..." and the meters show mic input and modem output levels.
6. Click **Stop** to end the transmission; an end-of-over (EOO) frame is sent automatically.

### Permissions

If you see "Failed to open audio devices", ensure PulseAudio is running and your user has access to the PulseAudio daemon. On most desktop Linux systems this works out of the box.

## Architecture

### Code structure

```
radae_decoder/
├── CMakeLists.txt              # Top-level build (GTK, PulseAudio, radae_nopy)
├── README.md
├── src/
│   ├── main.cpp                # GTK application, UI, event handlers
│   ├── rade_decoder.h/cpp      # RADAE decode pipeline (capture -> decode -> playback)
│   ├── rade_encoder.h/cpp      # RADAE encode pipeline (mic -> encode -> radio)
│   ├── audio_input.h/cpp       # PulseAudio device enumeration
│   ├── meter_widget.h/cpp      # Cairo-based bar meter widget
│   ├── spectrum_widget.h/cpp   # Cairo-based spectrum display
│   └── waterfall_widget.h/cpp  # Cairo-based waterfall display
└── radae_nopy/                 # RADAE codec library (C, builds librade + opus)
    ├── CMakeLists.txt
    ├── cmake/BuildOpus.cmake   # Downloads & builds Opus with FARGAN/LPCNet
    └── src/
        ├── rade_api.h          # Public C API
        ├── rade_rx.c           # Receiver (sync state machine, OFDM demod)
        ├── rade_enc/dec*.c     # Neural encoder/decoder + compiled weights
        ├── rade_ofdm.c         # OFDM modulation/demodulation
        ├── rade_acq.c          # Pilot acquisition & tracking
        └── ...
```

### Component overview

| Module | Responsibility |
|--------|---------------|
| **rade_decoder** | Complete real-time decode pipeline: PulseAudio capture (8 kHz), Hilbert transform (real to IQ), RADE receiver, FARGAN vocoder synthesis, PulseAudio playback (16 kHz). Runs on a dedicated thread with atomic status variables. |
| **rade_encoder** | Complete real-time encode pipeline: PulseAudio mic capture (16 kHz), LPCNet feature extraction, RADE transmitter (neural encoder + OFDM mod), PulseAudio playback to radio (8 kHz). Runs on a dedicated thread; TX output level controlled via atomic. |
| **audio_input** | PulseAudio device enumeration (sources and sinks) via the PulseAudio introspection API |
| **meter_widget** | Custom `GtkDrawingArea` widget; redraws at ~30 fps using Cairo; converts linear RMS to logarithmic dB; green-to-red gradient fill; peak-hold with decay |
| **main** | GTK application shell; connects signals; manages device combo boxes and TX level slider; starts/stops decoder/encoder; updates meters and status via GLib timer |
| **radae_nopy (librade)** | RADAE codec C library: OFDM mod/demod, pilot acquisition, neural encoder/decoder (GRU+Conv), bandpass filter. Neural network weights compiled directly into the binary (~47 MB). |

### Decode pipeline (RX)

```
PulseAudio Input (8 kHz mono)
  | pa_simple_read() -- blocking read, S16_LE mono
  v
Hilbert transform (127-tap Hamming-windowed FIR)
  -> RADE_COMP (complex IQ samples)
  v
rade_rx() -- pilot acquisition, OFDM demod, neural decoder
  -> 36-float feature vectors (12 per modem frame, when synced)
  v
FARGAN vocoder (fargan_synthesize)
  -> 160 float samples per frame @ 16 kHz (10 ms)
  v
pa_simple_write() -- PulseAudio playback, S16_LE mono
```

### Encode pipeline (TX)

```
PulseAudio Mic Input (16 kHz mono)
  | pa_simple_read() -- blocking read, S16_LE mono
  v
lpcnet_compute_single_frame_features()
  -> 36-float feature vector per 160 samples (10 ms)
  v
Accumulate 12 feature frames (432 floats, 120 ms)
  v
rade_tx() -- neural encoder, OFDM modulation
  -> 960 RADE_COMP samples @ 8 kHz (120 ms)
  v
Take real part, scale by TX level slider
  v
pa_simple_write() -- PulseAudio playback to radio, S16_LE mono
```

### Sync state machine

The RADE receiver has three states:

1. **SEARCH** -- Correlates incoming signal against pilot patterns across 40 frequency bins (+-100 Hz range, 2.5 Hz steps)
2. **CANDIDATE** -- Validates detected signal over multiple frames, refines timing and frequency
3. **SYNC** -- Locked and demodulating. Continuously tracks pilots. Loses sync if pilots fail for 3 seconds.

When sync is lost, the FARGAN vocoder is re-initialized so it can warm up cleanly when the next signal is acquired (5-frame warm-up via `fargan_cont()`).

### Thread model

- **RX processing thread** (RadaeDecoder): Runs the entire capture-decode-playback loop
- **TX processing thread** (RadaeEncoder): Runs the entire mic-capture-encode-playback loop
- **GTK main thread**: Reads atomic status variables at 30 Hz, updates meters, status label, and writes TX scale from the slider
- **Synchronization**: `std::atomic<float>` / `std::atomic<bool>` with relaxed ordering (lock-free)

## Troubleshooting

### "Package 'gtk+-3.0' not found"
Your pkg-config search path is missing the multiarch directory:
```bash
export PKG_CONFIG_PATH=/usr/lib/x86_64-linux-gnu/pkgconfig
```

### "Failed to open audio devices"
- Ensure PulseAudio is running: `pulseaudio --check` or `pactl info`.
- Verify devices exist: `pactl list sources short` (capture) and `pactl list sinks short` (playback).
- Try different devices from the dropdowns.

### No audio output / stuck on "Searching for signal..."
- Ensure the input device is actually receiving a RADAE modem signal at the expected bandwidth (~1.3 kHz around baseband).
- Check that the input level is reasonable (not clipping, not too quiet).
- The signal must contain RADAE OFDM pilots for the receiver to lock on.

### Build fails downloading Opus
The first build downloads Opus source from GitHub (~175 MB). Ensure you have an internet connection. If behind a proxy, set `http_proxy`/`https_proxy` environment variables.

### Build takes a long time
The neural network weight files (`rade_enc_data.c`, `rade_dec_data.c`) are ~24 MB and ~23 MB respectively. Compiling these takes significant time and memory. Use `make -j$(nproc)` for parallel builds.

## Technical details

### RADAE modem parameters
- **Sample rate**: 8000 Hz (modem), 16000 Hz (speech)
- **OFDM carriers**: 30
- **Bandwidth**: ~1.3 kHz
- **Modem frame**: 960 samples @ 8 kHz (120 ms)
- **Latent dimension**: 80 (neural autoencoder bottleneck)
- **Feature frames per modem frame**: 12 (3 latent vectors x 4 encoder stride)
- **Speech frame**: 160 samples @ 16 kHz (10 ms)

### PulseAudio configuration
- **API**: `pa_simple` (simple synchronous API)
- **Format**: `PA_SAMPLE_S16LE`
- **Capture rate**: 8 kHz (modem) / 16 kHz (mic); PulseAudio handles device rate conversion
- **Playback rate**: 16 kHz (speech) / 8 kHz (modem); PulseAudio handles device rate conversion
- **Channels**: 1 (mono)

## Demo tools

### RADE Demod: WAV RADE → WAV Speech Audio
Take a wav file off air and produce a demodulated wav file

Usage:
```
rade_demod [-v 0|1|2] <input.wav> <output.wav>
```

### RADE Modulate: WAV Speech Audio → WAV RADE
Take a wav file with speech in it and produce a RADE OFDM encoded output wav file ready for transmission.

Usage:
```
rade_modulate [-v 0|1|2] <intput.wav> <output.wav>
```

### Encode: WAV → IQ
```
sox ../voice.wav -r 16000 -t .s16 -c 1 - | \
  ./src/lpcnet_demo -features /dev/stdin - | \
  ./src/radae_tx > tx.iq
```

### Decode: IQ → WAV  
```
cat tx.iq | \
  ./src/radae_rx | \
  ./src/lpcnet_demo -fargan-synthesis /dev/stdin - | \
  sox -t .s16 -r 16000 -c 1 - decoded.wav
```

### Decode: WAV RADE → WAV (multiple steps)
```
usage: radae_rx [options]
  -h, --help           Show this help
  --model_name FILE    Path to model (ignored, uses built-in weights)
  -v LEVEL             Verbosity level (0, 1, or 2)
  --no-unsync          Disable automatic unsync
```

```
sox ../FDV_offair.wav -r 8000 -e float -b 32 -c 1 -t raw - | \
./src/real2iq | \
./src/radae_rx > features.f32
./src/lpcnet_demo -fargan-synthesis features.f32 - | \
sox -t .s16 -r 16000 -c 1 - decoded.wav
play decoded.wav
```

### Decode 8kHz int-16 samples from stdin (for OpenWebRX)

```
./webrx_rade_decode -h
usage: webrx_rade_decode [options]

  Reads 16-bit signed mono audio at 8000 Hz from stdin,
  decodes RADAE, and writes 16-bit signed mono audio
  at 8000 Hz to stdout.

options:
  -h, --help     Show this help
  -v LEVEL       Verbosity: 0=quiet  1=normal (default)  2=verbose
```

Test:
```sox FDV_FromRadio_20260125-080557_local.wav \
-t raw -b 16 -r 8000 -e signed-integer - | \
./webrx_rade_decode |sox -t raw -r 8000 -b 16 -e signed-integer -c 1 - output.wav
```

## Credits

- RADAE codec by David Rowe ([github.com/drowe67](https://github.com/drowe67))
- Opus/LPCNet/FARGAN by Xiph.Org / Amazon ([opus-codec.org](https://opus-codec.org/))
- Built with GTK 3 ([gtk.org](https://www.gtk.org/))
- Audio I/O via PulseAudio ([freedesktop.org/wiki/Software/PulseAudio](https://www.freedesktop.org/wiki/Software/PulseAudio/))
- Thanks David Rowe for help and encouragement.

---

**Platform**: Linux (Ubuntu 24.04 / Linux Mint)
