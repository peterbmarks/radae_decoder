/*---------------------------------------------------------------------------*\

  radae_headless.cpp

  RADAE headless transceiver - reads config from file, operates in TX or RX mode
  Uses PortAudio for audio I/O

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <sstream>
#include <portaudio.h>

#include "rade_decoder.h"
#include "rade_encoder.h"
#include "audio_input.h"

/* ── Configuration structure ──────────────────────────────────────────── */

struct Config {
    std::string fromradiodevice;
    std::string toradiodevice;
    std::string frommicrophone;
    std::string tospeakers;
    std::string call;
    std::string locator;
};

/* ── Global flag for signal handling ──────────────────────────────────── */

static volatile bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

/* ── Configuration file parsing ───────────────────────────────────────── */

bool parse_config_file(const char* filename, Config& config) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Could not open config file: %s\n", filename);
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        /* Skip empty lines and comments */
        if (line.empty() || line[0] == '#') {
            continue;
        }

        /* Parse key=value pairs */
        size_t pos = line.find('=');
        if (pos == std::string::npos) {
            continue;
        }

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        /* Trim whitespace */
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        /* Assign to config structure */
        if (key == "fromradiodevice") {
            config.fromradiodevice = value;
        } else if (key == "toradiodevice") {
            config.toradiodevice = value;
        } else if (key == "frommicrophone") {
            config.frommicrophone = value;
        } else if (key == "tospeakers") {
            config.tospeakers = value;
        } else if (key == "call") {
            config.call = value;
        } else if (key == "locator") {
            config.locator = value;
        }
    }

    return true;
}

/* ── Device enumeration ────────────────────────────────────────────────── */

void list_devices(void) {
    fprintf(stderr, "\n=== PortAudio Input Devices (for --fromradiodevice, --frommicrophone) ===\n");
    auto input_devices = AudioInput::enumerate_devices();
    if (input_devices.empty()) {
        fprintf(stderr, "  No input devices found\n");
    } else {
        for (const auto& dev : input_devices) {
            fprintf(stderr, "  %s\n", dev.hw_id.c_str());
            fprintf(stderr, "    Description: %s\n", dev.name.c_str());
        }
    }

    fprintf(stderr, "\n=== PortAudio Output Devices (for --toradiodevice, --tospeakers) ===\n");
    auto output_devices = AudioInput::enumerate_playback_devices();
    if (output_devices.empty()) {
        fprintf(stderr, "  No output devices found\n");
    } else {
        for (const auto& dev : output_devices) {
            fprintf(stderr, "  %s\n", dev.hw_id.c_str());
            fprintf(stderr, "    Description: %s\n", dev.name.c_str());
        }
    }
    fprintf(stderr, "\n");
}

/* ── Usage information ─────────────────────────────────────────────────── */

void usage(void) {
    fprintf(stderr, "usage: radae_headless [options]\n");
    fprintf(stderr, "  -h, --help                  Show this help\n");
    fprintf(stderr, "  -d, --devices               List available PortAudio devices and exit\n");
    fprintf(stderr, "  -c FILE                     Config file (default: radae_headless.conf)\n");
    fprintf(stderr, "  -t                          Transmit mode (default: receive mode)\n");
    fprintf(stderr, "  --fromradiodevice DEVICE    PortAudio device for radio input\n");
    fprintf(stderr, "  --toradiodevice DEVICE      PortAudio device for radio output\n");
    fprintf(stderr, "  --frommicrophone DEVICE     PortAudio device for microphone input\n");
    fprintf(stderr, "  --tospeakers DEVICE         PortAudio device for speaker output\n");
    fprintf(stderr, "  --call CALLSIGN             Callsign (e.g., VK3TPM)\n");
    fprintf(stderr, "  --locator LOCATOR           Grid locator (e.g., QF22ds)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Headless RADAE transceiver:\n");
    fprintf(stderr, "  RX mode: reads audio from --fromradiodevice, decodes, plays to --tospeakers\n");
    fprintf(stderr, "  TX mode: reads audio from --frommicrophone, encodes, sends to --toradiodevice\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Press Ctrl+C to stop.\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    Pa_Initialize();

    int opt;
    const char* config_file = "radae_headless.conf";
    bool transmit_mode = false;
    Config config;
    Config overrides;

    /* Track which options were explicitly set via command line */
    bool override_fromradio = false;
    bool override_toradio = false;
    bool override_frommicrophone = false;
    bool override_tospeakers = false;
    bool override_call = false;
    bool override_locator = false;

    static struct option long_options[] = {
        {"help",            no_argument,       NULL, 'h'},
        {"devices",         no_argument,       NULL, 'd'},
        {"fromradiodevice", required_argument, NULL, 'f'},
        {"toradiodevice",   required_argument, NULL, 'r'},
        {"frommicrophone",  required_argument, NULL, 'm'},
        {"tospeakers",      required_argument, NULL, 's'},
        {"call",            required_argument, NULL, 'a'},
        {"locator",         required_argument, NULL, 'l'},
        {NULL,              0,                 NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "hdtc:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            Pa_Terminate();
            return 0;
        case 'd':
            list_devices();
            Pa_Terminate();
            return 0;
        case 'c':
            config_file = optarg;
            break;
        case 't':
            transmit_mode = true;
            break;
        case 'f':
            overrides.fromradiodevice = optarg;
            override_fromradio = true;
            break;
        case 'r':
            overrides.toradiodevice = optarg;
            override_toradio = true;
            break;
        case 'm':
            overrides.frommicrophone = optarg;
            override_frommicrophone = true;
            break;
        case 's':
            overrides.tospeakers = optarg;
            override_tospeakers = true;
            break;
        case 'a':
            overrides.call = optarg;
            override_call = true;
            break;
        case 'l':
            overrides.locator = optarg;
            override_locator = true;
            break;
        default:
            usage();
            return 1;
        }
    }

    /* Parse config file */
    if (!parse_config_file(config_file, config)) {
        fprintf(stderr, "Failed to parse config file. Using command line values only.\n");
    }

    /* Apply command line overrides */
    if (override_fromradio) config.fromradiodevice = overrides.fromradiodevice;
    if (override_toradio) config.toradiodevice = overrides.toradiodevice;
    if (override_frommicrophone) config.frommicrophone = overrides.frommicrophone;
    if (override_tospeakers) config.tospeakers = overrides.tospeakers;
    if (override_call) config.call = overrides.call;
    if (override_locator) config.locator = overrides.locator;

    /* Validate configuration based on mode */
    if (transmit_mode) {
        if (config.frommicrophone.empty() || config.toradiodevice.empty()) {
            fprintf(stderr, "Error: TX mode requires --frommicrophone and --toradiodevice\n");
            usage();
            return 1;
        }
        fprintf(stderr, "Starting in TRANSMIT mode\n");
        fprintf(stderr, "  Microphone: %s\n", config.frommicrophone.c_str());
        fprintf(stderr, "  Radio out:  %s\n", config.toradiodevice.c_str());
    } else {
        if (config.fromradiodevice.empty() || config.tospeakers.empty()) {
            fprintf(stderr, "Error: RX mode requires --fromradiodevice and --tospeakers\n");
            usage();
            return 1;
        }
        fprintf(stderr, "Starting in RECEIVE mode\n");
        fprintf(stderr, "  Radio in:  %s\n", config.fromradiodevice.c_str());
        fprintf(stderr, "  Speakers:  %s\n", config.tospeakers.c_str());
    }

    if (!config.call.empty()) {
        fprintf(stderr, "  Call:      %s\n", config.call.c_str());
    }
    if (!config.locator.empty()) {
        fprintf(stderr, "  Locator:   %s\n", config.locator.c_str());
    }

    /* Set up signal handler for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Initialize RADE */
    rade_initialize();

    if (transmit_mode) {
        /* ── Transmit mode ─────────────────────────────────────────────── */
        RadaeEncoder encoder;

        fprintf(stderr, "Opening audio devices...\n");
        if (!encoder.open(config.frommicrophone, config.toradiodevice)) {
            fprintf(stderr, "Error: Failed to open encoder devices\n");
            rade_finalize();
            return 1;
        }

        fprintf(stderr, "Starting encoder...\n");
        encoder.start();

        fprintf(stderr, "Running... Press Ctrl+C to stop\n");
        while (g_running && encoder.is_running()) {
            sleep(1);
            /* Could print status here if desired */
            float input_level = encoder.get_input_level();
            float output_level = encoder.get_output_level();
            fprintf(stderr, "\rInput: %.2f  Output: %.2f  ", input_level, output_level);
            fflush(stderr);
        }
        fprintf(stderr, "\n");

        fprintf(stderr, "Stopping encoder...\n");
        encoder.stop();
        encoder.close();

    } else {
        /* ── Receive mode ──────────────────────────────────────────────── */
        RadaeDecoder decoder;

        fprintf(stderr, "Opening audio devices...\n");
        if (!decoder.open(config.fromradiodevice, config.tospeakers)) {
            fprintf(stderr, "Error: Failed to open decoder devices\n");
            rade_finalize();
            return 1;
        }

        fprintf(stderr, "Starting decoder...\n");
        decoder.start();

        fprintf(stderr, "Running... Press Ctrl+C to stop\n");
        while (g_running && decoder.is_running()) {
            sleep(1);
            /* Print status */
            bool synced = decoder.is_synced();
            float snr = decoder.snr_dB();
            float freq_offset = decoder.freq_offset();
            float input_level = decoder.get_input_level();
            float output_level = decoder.get_output_level_left();

            fprintf(stderr, "\r%s SNR: %.1f dB  Freq: %+.1f Hz  In: %.2f  Out: %.2f  ",
                    synced ? "SYNC" : "----", snr, freq_offset, input_level, output_level);
            fflush(stderr);
        }
        fprintf(stderr, "\n");

        fprintf(stderr, "Stopping decoder...\n");
        decoder.stop();
        decoder.close();
    }

    /* Cleanup */
    rade_finalize();
    Pa_Terminate();

    fprintf(stderr, "Shutdown complete\n");
    return 0;
}
