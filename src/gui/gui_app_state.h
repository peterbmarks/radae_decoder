#pragma once

#include <gtk/gtk.h>
#include <vector>
#include <string>
#include <cstdint>

#include "../src/audio/audio_input.h"
#include "../src/radae_top/rade_decoder.h"
#include "../src/radae_top/rade_encoder.h"
#include "../src/wav/wav_recorder.h"
#include "../network/freedv_reporter.h"

/* ── globals (single-window app) ────────────────────────────────────────── */

extern RadaeDecoder*            g_decoder;
extern RadaeEncoder*            g_encoder;
extern GtkWidget*               g_tx_switch;       // TX mode toggle
extern GtkWidget*               g_bpf_switch;      // TX BPF toggle
extern std::vector<AudioDevice> g_input_devices;
extern std::vector<AudioDevice> g_output_devices;
extern GtkWidget*               g_input_combo;     // input device selector
extern GtkWidget*               g_output_combo;    // output device selector
extern std::vector<AudioDevice> g_tx_input_devices;
extern std::vector<AudioDevice> g_tx_output_devices;
extern GtkWidget*               g_tx_input_combo;  // transmit mic input selector
extern GtkWidget*               g_tx_output_combo; // transmit radio output selector
extern GtkWidget*               g_btn;             // start / stop
extern GtkWidget*               g_record_btn;      // record / stop-record
extern WavRecorder*             g_recorder;        // active WAV recorder
extern bool                     g_recording;       // recording in progress
extern GtkWidget*               g_meter_in;        // input level meter
extern GtkWidget*               g_meter_out;       // output level meter
extern GtkWidget*               g_spectrum;        // spectrum widget
extern GtkWidget*               g_waterfall;       // waterfall widget
extern GtkWidget*               g_status;          // status label
extern GtkWidget*               g_rig_status_lbl;  // rig status line under waterfall
extern GtkWidget*               g_window;          // main window
extern GtkWidget*               g_settings_dlg;    // settings dialog
extern GtkWidget*               g_rig_dlg;         // rig control dialog
extern GtkWidget*               g_callsign_entry;  // station callsign
extern GtkWidget*               g_gridsquare_entry;// station gridsquare
extern GtkWidget*               g_mic_slider;      // TX mic input level slider
extern GtkWidget*               g_tx_slider;       // TX output level slider
extern guint                    g_timer;           // meter update timer
extern bool                     g_updating_combos; // guard programmatic changes
extern FreeDVReporter*          g_reporter;        // FreeDV Reporter client
extern std::string              g_last_rx_callsign;// last callsign sent to reporter
extern uint64_t                 g_last_reporter_freq; // last frequency sent to reporter
