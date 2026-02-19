/*---------------------------------------------------------------------------*\

  radae_headless.cpp

  RADAE headless transceiver - reads config from file, operates in TX or RX mode

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

#include "rade_decoder.h"
#include "rade_encoder.h"
#include "audio_input.h"

/* ── Configuration structure ──────────────────────────────────────────── */

struct Config {
    std::string fromradio;
    std::string toradio;
    std::string frommic;
    std::string tospeaker;
    std::string call;
};

/* ── Global flag for signal handling ──────────────────────────────────── */

static volatile bool g_running = true;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

/* ── Configuration file I/O ───────────────────────────────────────────── */

bool write_config_file(const char* filename, const Config& config) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Could not write config file: %s\n", filename);
        return false;
    }
    file << "# radae_headless configuration\n";
    if (!config.fromradio.empty()) file << "fromradio=" << config.fromradio << '\n';
    if (!config.toradio.empty())   file << "toradio="   << config.toradio   << '\n';
    if (!config.frommic.empty())   file << "frommic="   << config.frommic   << '\n';
    if (!config.tospeaker.empty()) file << "tospeaker=" << config.tospeaker << '\n';
    if (!config.call.empty())      file << "call="      << config.call      << '\n';
    return true;
}

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
        if (key == "fromradio") {
            config.fromradio = value;
        } else if (key == "toradio") {
            config.toradio = value;
        } else if (key == "frommic") {
            config.frommic = value;
        } else if (key == "tospeaker") {
            config.tospeaker = value;
        } else if (key == "call") {
            config.call = value;
        }
    }

    return true;
}

/* ── Device enumeration ────────────────────────────────────────────────── */

void list_devices(void) {
    fprintf(stderr, "\n=== Input Devices (for --fromradio, --frommic) ===\n");
    auto input_devices = AudioInput::enumerate_devices();
    if (input_devices.empty()) {
        fprintf(stderr, "  No input devices found\n");
    } else {
        for (const auto& dev : input_devices) {
            fprintf(stderr, "  %s\n", dev.hw_id.c_str());
            fprintf(stderr, "    Description: %s\n", dev.name.c_str());
        }
    }

    fprintf(stderr, "\n=== Output Devices (for --toradio, --tospeaker) ===\n");
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
    fprintf(stderr, "  -d, --devices               List available audio devices and exit\n");
    fprintf(stderr, "  -c FILE                     Config file (default: radae_headless.conf)\n");
    fprintf(stderr, "  -t                          Transmit mode (default: receive mode)\n");
    fprintf(stderr, "  --fromradio DEVICE    Audio device for radio input\n");
    fprintf(stderr, "  --toradio DEVICE      Audio device for radio output\n");
    fprintf(stderr, "  --frommic DEVICE     Audio device for microphone input\n");
    fprintf(stderr, "  --tospeaker DEVICE         Audio device for speaker output\n");
    fprintf(stderr, "  --call CALLSIGN             Callsign (e.g., VK3TPM)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Headless RADAE transceiver:\n");
    fprintf(stderr, "  RX mode: reads audio from --fromradio, decodes, plays to --tospeaker\n");
    fprintf(stderr, "  TX mode: reads audio from --frommic, encodes, sends to --toradio\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Press Ctrl+C to stop.\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    audio_init();

    int opt;
    const char* config_file = "radae_headless.conf";
    bool transmit_mode = false;
    Config config;
    Config overrides;

    /* Track which options were explicitly set via command line */
    bool override_fromradio = false;
    bool override_toradio = false;
    bool override_frommic = false;
    bool override_tospeaker = false;
    bool override_call = false;

    static struct option long_options[] = {
        {"help",            no_argument,       NULL, 'h'},
        {"devices",         no_argument,       NULL, 'd'},
        {"fromradio", required_argument, NULL, 'f'},
        {"toradio",   required_argument, NULL, 'r'},
        {"frommic",  required_argument, NULL, 'm'},
        {"tospeaker",      required_argument, NULL, 's'},
        {"call",            required_argument, NULL, 'a'},
        {NULL,              0,                 NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "hdtc:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage();
            audio_terminate();
            return 0;
        case 'd':
            list_devices();
            audio_terminate();
            return 0;
        case 'c':
            config_file = optarg;
            break;
        case 't':
            transmit_mode = true;
            break;
        case 'f':
            overrides.fromradio = optarg;
            override_fromradio = true;
            break;
        case 'r':
            overrides.toradio = optarg;
            override_toradio = true;
            break;
        case 'm':
            overrides.frommic = optarg;
            override_frommic = true;
            break;
        case 's':
            overrides.tospeaker = optarg;
            override_tospeaker = true;
            break;
        case 'a':
            overrides.call = optarg;
            override_call = true;
            break;
        default:
            usage();
            return 1;
        }
    }

    /* Parse config file, or create it from CLI options if not found */
    {
        std::ifstream probe(config_file);
        bool file_found = probe.is_open();
        probe.close();

        if (file_found) {
            if (!parse_config_file(config_file, config)) {
                fprintf(stderr, "Failed to parse config file '%s'.\n", config_file);
            }
        } else {
            bool any_override = override_fromradio || override_toradio ||
                                override_frommic   || override_tospeaker || override_call;
            if (any_override) {
                if (write_config_file(config_file, overrides))
                    fprintf(stderr, "Config file '%s' not found — created from command line options.\n",
                            config_file);
            } else {
                fprintf(stderr, "Warning: config file '%s' not found and no options given.\n",
                        config_file);
            }
        }
    }

    /* Apply command line overrides */
    if (override_fromradio) config.fromradio = overrides.fromradio;
    if (override_toradio) config.toradio = overrides.toradio;
    if (override_frommic) config.frommic = overrides.frommic;
    if (override_tospeaker) config.tospeaker = overrides.tospeaker;
    if (override_call) config.call = overrides.call;

    /* Validate configuration based on mode */
    if (transmit_mode) {
        if (config.frommic.empty() || config.toradio.empty()) {
            fprintf(stderr, "Error: TX mode requires --frommic and --toradio\n");
            usage();
            return 1;
        }
        fprintf(stderr, "Starting in TRANSMIT mode\n");
        fprintf(stderr, "  Microphone: %s\n", config.frommic.c_str());
        fprintf(stderr, "  Radio out:  %s\n", config.toradio.c_str());
    } else {
        if (config.fromradio.empty() || config.tospeaker.empty()) {
            fprintf(stderr, "Error: RX mode requires --fromradio and --tospeaker\n");
            usage();
            return 1;
        }
        fprintf(stderr, "Starting in RECEIVE mode\n");
        fprintf(stderr, "  Radio in:  %s\n", config.fromradio.c_str());
        fprintf(stderr, "  Speakers:  %s\n", config.tospeaker.c_str());
    }

    if (!config.call.empty()) {
        fprintf(stderr, "  Call:      %s\n", config.call.c_str());
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
        if (!encoder.open(config.frommic, config.toradio)) {
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
        if (!decoder.open(config.fromradio, config.tospeaker)) {
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
    audio_terminate();

    fprintf(stderr, "Shutdown complete\n");
    return 0;
}
