#include "gui_app_state.h"
#include "gui_activate.h"
#include "../src/audio/audio_input.h"

/* ── global definitions (single-window app) ─────────────────────────────── */

RadaeDecoder*            g_decoder            = nullptr;
RadaeEncoder*            g_encoder            = nullptr;
GtkWidget*               g_tx_switch          = nullptr;   // TX mode toggle
GtkWidget*               g_bpf_switch         = nullptr;   // TX BPF toggle
std::vector<AudioDevice> g_input_devices;
std::vector<AudioDevice> g_output_devices;
GtkWidget*               g_input_combo        = nullptr;   // input device selector
GtkWidget*               g_output_combo       = nullptr;   // output device selector
std::vector<AudioDevice> g_tx_input_devices;
std::vector<AudioDevice> g_tx_output_devices;
GtkWidget*               g_tx_input_combo     = nullptr;   // transmit mic input selector
GtkWidget*               g_tx_output_combo    = nullptr;   // transmit radio output selector
GtkWidget*               g_btn                = nullptr;   // start / stop
GtkWidget*               g_record_btn         = nullptr;   // record / stop-record
WavRecorder*             g_recorder           = nullptr;   // active WAV recorder
bool                     g_recording          = false;     // recording in progress
GtkWidget*               g_meter_in           = nullptr;   // input level meter
GtkWidget*               g_meter_out          = nullptr;   // output level meter
GtkWidget*               g_spectrum           = nullptr;   // spectrum widget
GtkWidget*               g_waterfall          = nullptr;   // waterfall widget
GtkWidget*               g_status             = nullptr;   // status label
GtkWidget*               g_rig_status_lbl     = nullptr;   // rig status line under waterfall
GtkWidget*               g_window             = nullptr;   // main window
GtkWidget*               g_settings_dlg       = nullptr;   // settings dialog
GtkWidget*               g_rig_dlg            = nullptr;   // rig control dialog
GtkWidget*               g_callsign_entry     = nullptr;   // station callsign
GtkWidget*               g_gridsquare_entry   = nullptr;   // station gridsquare
GtkWidget*               g_mic_slider         = nullptr;   // TX mic input level slider
GtkWidget*               g_tx_slider          = nullptr;   // TX output level slider
guint                    g_timer              = 0;         // meter update timer
bool                     g_updating_combos    = false;     // guard programmatic changes
FreeDVReporter*          g_reporter           = nullptr;   // FreeDV Reporter client
std::string              g_last_rx_callsign;               // last callsign sent to reporter
uint64_t                 g_last_reporter_freq = 0;         // last frequency sent to reporter

/* ── entry point ────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
    audio_init();

    GtkApplication* app = gtk_application_new("org.simpledecoder.RADAEDecoder",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    audio_terminate();
    return rc;
}
