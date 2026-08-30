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
#include <math.h>
#include <getopt.h>
#include <signal.h>
#include <unistd.h>
#include <string>
#include <fstream>
#include <sstream>

#include "rade_api.h"
#include "../src/radae_top/rade_decoder.h"
#include "../src/radae_top/rade_encoder.h"
#include "../src/audio/audio_input.h"
#include "device_picker.h"

/* ── Configuration structure ──────────────────────────────────────────── */

struct Config {
    std::string fromradio;
    std::string toradio;
    std::string frommic;
    std::string tospeaker;
    std::string call;
};

/* ── Global flags for signal handling ─────────────────────────────────── */

/* MODE_NONE means no switch has been requested; SIGUSR1/SIGUSR2 request a
   runtime switch to transmit/receive without restarting the process. */
enum { MODE_NONE = -1, MODE_RECEIVE = 0, MODE_TRANSMIT = 1 };

static volatile bool g_running = true;
static volatile sig_atomic_t g_mode_request = MODE_NONE;

void signal_handler(int signum) {
    (void)signum;
    g_running = false;
}

void signal_handler_tx(int signum) {
    (void)signum;
    g_mode_request = MODE_TRANSMIT;
}

void signal_handler_rx(int signum) {
    (void)signum;
    g_mode_request = MODE_RECEIVE;
}

/* ── Configuration file I/O ───────────────────────────────────────────── */

bool write_config_file(const char* filename, const Config& config,
                      bool include_empty = false) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        fprintf(stderr, "Error: Could not write config file: %s\n", filename);
        return false;
    }
    file << "# radae_headless configuration\n";
    if (include_empty || !config.fromradio.empty()) file << "fromradio=" << config.fromradio << '\n';
    if (include_empty || !config.toradio.empty())   file << "toradio="   << config.toradio   << '\n';
    if (include_empty || !config.frommic.empty())   file << "frommic="   << config.frommic   << '\n';
    if (include_empty || !config.tospeaker.empty()) file << "tospeaker=" << config.tospeaker << '\n';
    if (!config.call.empty())                       file << "call="      << config.call      << '\n';
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

/* ── Level formatting ──────────────────────────────────────────────────── */

/* The encoder and decoder report levels as RMS amplitude relative to digital
   full scale, so a healthy signal sits around 0.03 and the raw linear figure
   looks alarmingly small even when the level is fine.  Audio levels are read
   logarithmically, so map onto the same -60..0 dBFS range the GUI meter uses
   and show that position as a percentage of full scale. */

static const float LEVEL_DB_MIN = -60.0f;

float level_dbfs(float rms) {
    if (rms < 1e-6f) return LEVEL_DB_MIN;
    float db = 20.0f * log10f(rms);
    if (db < LEVEL_DB_MIN) return LEVEL_DB_MIN;
    if (db > 0.0f)         return 0.0f;
    return db;
}

int level_percent(float rms) {
    return (int)lrintf((level_dbfs(rms) - LEVEL_DB_MIN) / -LEVEL_DB_MIN * 100.0f);
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

/* ── First-run setup wizard ────────────────────────────────────────────── */

/* Walk the user through choosing each of the four audio devices.  Returns
   false if the user backed out, in which case nothing should be written. */
bool run_setup_wizard(Config& config) {
    auto inputs  = AudioInput::enumerate_devices();
    auto outputs = AudioInput::enumerate_playback_devices();

    if (inputs.empty() && outputs.empty()) {
        fprintf(stderr, "Error: no audio devices found.\n");
        return false;
    }

    struct Step {
        const char*  title;
        const char*  hint;
        std::string* target;
        bool         is_input;
    };

    const Step steps[] = {
        { "fromradio", "Audio input carrying the RADAE signal from the radio",
          &config.fromradio, true  },
        { "tospeaker", "Audio output for the decoded speech",
          &config.tospeaker, false },
        { "frommic",   "Audio input from your microphone, used when transmitting",
          &config.frommic,   true  },
        { "toradio",   "Audio output feeding the radio's transmitter",
          &config.toradio,   false },
    };

    fprintf(stderr, "\nNo configuration found - let's set up your audio devices.\n\n");

    for (const Step& step : steps) {
        const auto& devices = step.is_input ? inputs : outputs;
        if (devices.empty()) {
            fprintf(stderr, "%s: no %s devices available, skipping\n",
                    step.title, step.is_input ? "input" : "output");
            step.target->clear();
            continue;
        }

        int choice = device_picker::pick(step.title, step.hint, devices);
        if (choice == device_picker::PICK_CANCEL) {
            fprintf(stderr, "\nSetup cancelled - no configuration written.\n");
            return false;
        }
        if (choice == device_picker::PICK_NONE) {
            step.target->clear();
        } else {
            *step.target = devices[choice].hw_id;
        }
    }

    fprintf(stderr, "\n");
    return true;
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
    fprintf(stderr, "Send SIGUSR1 to switch to transmit, SIGUSR2 to switch to receive\n");
    fprintf(stderr, "(both fromradio/tospeaker and frommic/toradio must be configured\n");
    fprintf(stderr, "to switch at runtime). Press Ctrl+C to stop.\n");
}

/* ── Mode run loops ────────────────────────────────────────────────────── */

/* Each returns when told to stop (g_running cleared), a switch to the other
   mode is requested, or the encoder/decoder stops itself (e.g. device
   error) - the caller distinguishes these via g_running / g_mode_request. */

int run_transmit_mode(const Config& config) {
    RadaeEncoder encoder;

    fprintf(stderr, "Opening audio devices...\n");
    if (!encoder.open(config.frommic, config.toradio)) {
        fprintf(stderr, "Error: Failed to open encoder devices\n");
        return -1;
    }

    fprintf(stderr, "Starting encoder...\n");
    encoder.start();

    fprintf(stderr, "Running in TRANSMIT mode... Press Ctrl+C to stop\n");
    while (g_running && g_mode_request == MODE_NONE && encoder.is_running()) {
        sleep(1);
        float input_level = encoder.get_input_level();
        float output_level = encoder.get_output_level();
        fprintf(stderr, "\rIn: %3d%% (%3.0f dBFS)  Out: %3d%% (%3.0f dBFS)  ",
                level_percent(input_level),  level_dbfs(input_level),
                level_percent(output_level), level_dbfs(output_level));
        fflush(stderr);
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "Stopping encoder...\n");
    encoder.stop();
    encoder.close();
    return 0;
}

int run_receive_mode(const Config& config) {
    RadaeDecoder decoder;

    fprintf(stderr, "Opening audio devices...\n");
    if (!decoder.open(config.fromradio, config.tospeaker)) {
        fprintf(stderr, "Error: Failed to open decoder devices\n");
        return -1;
    }

    fprintf(stderr, "Starting decoder...\n");
    decoder.start();

    fprintf(stderr, "Running in RECEIVE mode... Press Ctrl+C to stop\n");
    while (g_running && g_mode_request == MODE_NONE && decoder.is_running()) {
        sleep(1);
        bool synced = decoder.is_synced();
        float snr = decoder.snr_dB();
        float freq_offset = decoder.freq_offset();
        float input_level = decoder.get_input_level();
        float output_level = decoder.get_output_level_left();

        fprintf(stderr,
                "\r%s SNR: %.1f dB  Freq: %+.1f Hz  "
                "In: %3d%% (%3.0f dBFS)  Out: %3d%% (%3.0f dBFS)  ",
                synced ? "SYNC" : "----", snr, freq_offset,
                level_percent(input_level),  level_dbfs(input_level),
                level_percent(output_level), level_dbfs(output_level));
        fflush(stderr);
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "Stopping decoder...\n");
    decoder.stop();
    decoder.close();
    return 0;
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
            } else if (!transmit_mode && device_picker::available()) {
                /* Interactive first run: ask for each device, then save and
                   carry straight on into receive mode. */
                if (!run_setup_wizard(config)) {
                    audio_terminate();
                    return 1;
                }
                if (!write_config_file(config_file, config, true)) {
                    audio_terminate();
                    return 1;
                }
                fprintf(stderr, "Saved configuration to '%s'.\n", config_file);

                if (config.fromradio.empty() || config.tospeaker.empty()) {
                    fprintf(stderr,
                            "Receiving needs both fromradio and tospeaker.\n"
                            "Edit '%s', or delete it and run again to choose "
                            "different devices.\n", config_file);
                    audio_terminate();
                    return 1;
                }
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

    /* Validate configuration based on initial mode; device info for the
       starting mode is printed by the run loop below */
    if (transmit_mode) {
        if (config.frommic.empty() || config.toradio.empty()) {
            fprintf(stderr, "Error: TX mode requires --frommic and --toradio\n");
            usage();
            return 1;
        }
    } else {
        if (config.fromradio.empty() || config.tospeaker.empty()) {
            fprintf(stderr, "Error: RX mode requires --fromradio and --tospeaker\n");
            usage();
            return 1;
        }
    }

    if (!config.call.empty()) {
        fprintf(stderr, "  Call:      %s\n", config.call.c_str());
    }

    /* Set up signal handlers for graceful shutdown and mode switching */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGUSR1, signal_handler_tx);
    signal(SIGUSR2, signal_handler_rx);

    /* Initialize RADE */
    rade_initialize();

    bool current_transmit = transmit_mode;
    int result = 0;

    while (g_running) {
        g_mode_request = MODE_NONE;

        if (current_transmit) {
            if (config.frommic.empty() || config.toradio.empty()) {
                fprintf(stderr, "Error: TX mode requires frommic and toradio - cannot switch\n");
                result = 1;
                break;
            }
            fprintf(stderr, "Starting in TRANSMIT mode\n");
            fprintf(stderr, "  Microphone: %s\n", config.frommic.c_str());
            fprintf(stderr, "  Radio out:  %s\n", config.toradio.c_str());
            if (run_transmit_mode(config) != 0) {
                result = 1;
                break;
            }
        } else {
            if (config.fromradio.empty() || config.tospeaker.empty()) {
                fprintf(stderr, "Error: RX mode requires fromradio and tospeaker - cannot switch\n");
                result = 1;
                break;
            }
            fprintf(stderr, "Starting in RECEIVE mode\n");
            fprintf(stderr, "  Radio in:  %s\n", config.fromradio.c_str());
            fprintf(stderr, "  Speakers:  %s\n", config.tospeaker.c_str());
            if (run_receive_mode(config) != 0) {
                result = 1;
                break;
            }
        }

        if (!g_running) break;

        if (g_mode_request == MODE_TRANSMIT) {
            current_transmit = true;
        } else if (g_mode_request == MODE_RECEIVE) {
            current_transmit = false;
        } else {
            /* Loop exited on its own (device stopped), not a mode switch */
            break;
        }
    }

    /* Cleanup */
    rade_finalize();
    audio_terminate();

    fprintf(stderr, "Shutdown complete\n");
    return result;
}
