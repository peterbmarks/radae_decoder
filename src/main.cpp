#include <gtk/gtk.h>
#include <vector>
#include <string>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include "audio_input.h"
#include "meter_widget.h"
#include "rade_decoder.h"
#include "rade_encoder.h"
#include "spectrum_widget.h"
#include "waterfall_widget.h"
#include "wav_recorder.h"

/* ── globals (single-window app) ────────────────────────────────────────── */

static RadaeDecoder*            g_decoder            = nullptr;
static RadaeEncoder*            g_encoder            = nullptr;
static GtkWidget*               g_tx_switch          = nullptr;   // TX mode toggle
static GtkWidget*               g_bpf_switch         = nullptr;   // TX BPF toggle
static std::vector<AudioDevice> g_input_devices;
static std::vector<AudioDevice> g_output_devices;
static GtkWidget*               g_input_combo        = nullptr;   // input device selector
static GtkWidget*               g_output_combo       = nullptr;   // output device selector
static std::vector<AudioDevice> g_tx_input_devices;
static std::vector<AudioDevice> g_tx_output_devices;
static GtkWidget*               g_tx_input_combo     = nullptr;   // transmit mic input selector
static GtkWidget*               g_tx_output_combo    = nullptr;   // transmit radio output selector
static GtkWidget*               g_btn                = nullptr;   // start / stop
static GtkWidget*               g_record_btn         = nullptr;   // record / stop-record
static WavRecorder*             g_recorder           = nullptr;   // active WAV recorder
static bool                     g_recording          = false;     // recording in progress
static GtkWidget*               g_meter_in           = nullptr;   // input level meter
static GtkWidget*               g_meter_out          = nullptr;   // output level meter
static GtkWidget*               g_spectrum           = nullptr;   // spectrum widget
static GtkWidget*               g_waterfall          = nullptr;   // waterfall widget
static GtkWidget*               g_status             = nullptr;   // status label
static GtkWidget*               g_settings_dlg       = nullptr;   // settings dialog
static GtkWidget*               g_callsign_entry     = nullptr;   // station callsign
static GtkWidget*               g_gridsquare_entry   = nullptr;   // station gridsquare
static GtkWidget*               g_mic_slider         = nullptr;   // TX mic input level slider
static GtkWidget*               g_tx_slider          = nullptr;   // TX output level slider
static guint                    g_timer              = 0;         // meter update timer
static bool                     g_updating_combos    = false;     // guard programmatic changes

/* ── config persistence ─────────────────────────────────────────────────── */

static std::string config_path()
{
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.config";
    mkdir(dir.c_str(), 0755);
    return dir + "/radae-decoder.conf";
}

static void save_config()
{
    int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));

    std::string in_name, out_name;
    if (in_idx >= 0 && in_idx < static_cast<int>(g_input_devices.size()))
        in_name = g_input_devices[static_cast<size_t>(in_idx)].name;
    if (out_idx >= 0 && out_idx < static_cast<int>(g_output_devices.size()))
        out_name = g_output_devices[static_cast<size_t>(out_idx)].name;

    int tx_in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_input_combo));
    int tx_out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_output_combo));

    std::string tx_in_name, tx_out_name;
    if (tx_in_idx >= 0 && tx_in_idx < static_cast<int>(g_tx_input_devices.size()))
        tx_in_name = g_tx_input_devices[static_cast<size_t>(tx_in_idx)].name;
    if (tx_out_idx >= 0 && tx_out_idx < static_cast<int>(g_tx_output_devices.size()))
        tx_out_name = g_tx_output_devices[static_cast<size_t>(tx_out_idx)].name;

    std::ofstream f(config_path());
    if (f) {
        f << "input=" << in_name << '\n';
        f << "output=" << out_name << '\n';
        f << "tx_input=" << tx_in_name << '\n';
        f << "tx_output=" << tx_out_name << '\n';
        f << "tx_level=" << static_cast<int>(gtk_range_get_value(GTK_RANGE(g_tx_slider))) << '\n';
        f << "mic_level=" << static_cast<int>(gtk_range_get_value(GTK_RANGE(g_mic_slider))) << '\n';
        f << "bpf_enabled=" << (g_bpf_switch && gtk_switch_get_active(GTK_SWITCH(g_bpf_switch)) ? 1 : 0) << '\n';
        const char* cs = g_callsign_entry ? gtk_entry_get_text(GTK_ENTRY(g_callsign_entry)) : "";
        f << "callsign=" << cs << '\n';
        const char* gs = g_gridsquare_entry ? gtk_entry_get_text(GTK_ENTRY(g_gridsquare_entry)) : "";
        f << "gridsquare=" << gs << '\n';
    }
}

/* Try to select saved devices in the combos.  Returns true if both found. */
static bool restore_config()
{
    std::ifstream f(config_path());
    if (!f) return false;

    std::string saved_in, saved_out, saved_tx_in, saved_tx_out, saved_callsign, saved_gridsquare;
    int saved_tx_level = -1;
    int saved_mic_level = -1;
    int saved_bpf_enabled = -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "input=") == 0)
            saved_in = line.substr(6);
        else if (line.compare(0, 7, "output=") == 0)
            saved_out = line.substr(7);
        else if (line.compare(0, 9, "tx_input=") == 0)
            saved_tx_in = line.substr(9);
        else if (line.compare(0, 10, "tx_output=") == 0)
            saved_tx_out = line.substr(10);
        else if (line.compare(0, 9, "tx_level=") == 0)
            saved_tx_level = std::stoi(line.substr(9));
        else if (line.compare(0, 10, "mic_level=") == 0)
            saved_mic_level = std::stoi(line.substr(10));
        else if (line.compare(0, 12, "bpf_enabled=") == 0)
            saved_bpf_enabled = std::stoi(line.substr(12));
        else if (line.compare(0, 9, "callsign=") == 0)
            saved_callsign = line.substr(9);
        else if (line.compare(0, 11, "gridsquare=") == 0)
            saved_gridsquare = line.substr(11);
    }

    if (saved_in.empty() && saved_out.empty()) return false;

    int in_idx = -1, out_idx = -1;
    for (size_t i = 0; i < g_input_devices.size(); i++) {
        if (g_input_devices[i].name == saved_in) {
            in_idx = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < g_output_devices.size(); i++) {
        if (g_output_devices[i].name == saved_out) {
            out_idx = static_cast<int>(i);
            break;
        }
    }

    int tx_in_idx = -1, tx_out_idx = -1;
    for (size_t i = 0; i < g_tx_input_devices.size(); i++) {
        if (g_tx_input_devices[i].name == saved_tx_in) {
            tx_in_idx = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < g_tx_output_devices.size(); i++) {
        if (g_tx_output_devices[i].name == saved_tx_out) {
            tx_out_idx = static_cast<int>(i);
            break;
        }
    }

    g_updating_combos = true;
    if (in_idx >= 0)     gtk_combo_box_set_active(GTK_COMBO_BOX(g_input_combo), in_idx);
    if (out_idx >= 0)    gtk_combo_box_set_active(GTK_COMBO_BOX(g_output_combo), out_idx);
    if (tx_in_idx >= 0)  gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_input_combo), tx_in_idx);
    if (tx_out_idx >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_output_combo), tx_out_idx);
    g_updating_combos = false;

    if (saved_tx_level >= 0 && saved_tx_level <= 100)
        gtk_range_set_value(GTK_RANGE(g_tx_slider), saved_tx_level);
    if (saved_mic_level >= 0 && saved_mic_level <= 100)
        gtk_range_set_value(GTK_RANGE(g_mic_slider), saved_mic_level);
    if (saved_bpf_enabled >= 0 && g_bpf_switch)
        gtk_switch_set_active(GTK_SWITCH(g_bpf_switch), saved_bpf_enabled != 0);
    if (!saved_callsign.empty() && g_callsign_entry)
        gtk_entry_set_text(GTK_ENTRY(g_callsign_entry), saved_callsign.c_str());
    if (!saved_gridsquare.empty() && g_gridsquare_entry)
        gtk_entry_set_text(GTK_ENTRY(g_gridsquare_entry), saved_gridsquare.c_str());

    return (in_idx >= 0 && out_idx >= 0);
}

/* ── helpers ────────────────────────────────────────────────────────────── */

static void set_status(const char* msg)
{
    gtk_label_set_text(GTK_LABEL(g_status), msg);
}

/* change the button label AND its CSS class in one shot */
static void set_btn_state(bool capturing)
{
    GtkStyleContext* ctx = gtk_widget_get_style_context(g_btn);
    if (capturing) {
        gtk_style_context_remove_class(ctx, "start-btn");
        gtk_style_context_add_class   (ctx, "stop-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Stop");
    } else {
        gtk_style_context_remove_class(ctx, "stop-btn");
        gtk_style_context_add_class   (ctx, "start-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Start");
    }
}

/* ── TX level slider callbacks ─────────────────────────────────────────── */

static void on_mic_level_changed(GtkRange* range, gpointer /*data*/)
{
    double pct = gtk_range_get_value(range);
    float gain = static_cast<float>(pct / 100.0 * 2.0);   /* 0-100% → 0.0-2.0 */
    if (g_encoder)
        g_encoder->set_mic_gain(gain);
}

static void on_tx_level_changed(GtkRange* range, gpointer /*data*/)
{
    double pct = gtk_range_get_value(range);
    float scale = static_cast<float>(pct / 100.0 * 32767.0);
    if (g_encoder)
        g_encoder->set_tx_scale(scale);
}

/* ── decoder control ───────────────────────────────────────────────────── */

static void stop_all()
{
    /* Stop threads first so the EOO frame is flushed into the recorder,
       then detach the recorder once the threads have finished. */
    if (g_decoder) { g_decoder->stop(); g_decoder->close(); }
    if (g_encoder) { g_encoder->stop(); g_encoder->close(); }

    if (g_decoder) g_decoder->set_recorder(nullptr);
    if (g_encoder) g_encoder->set_recorder(nullptr);
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    if (g_meter_in)  meter_widget_update(g_meter_in, 0.f);
    if (g_meter_out) meter_widget_update(g_meter_out, 0.f);
    if (g_spectrum)  spectrum_widget_update(g_spectrum, nullptr, 0, 8000.f);
    if (g_waterfall) waterfall_widget_update(g_waterfall, nullptr, 0, 8000.f);
    set_btn_state(false);
}

/* timer tick – update meter + status at ~30 fps */
static gboolean on_meter_tick(gpointer /*data*/)
{
    /* ── TX mode ─────────────────────────────────────────────────────── */
    if (g_encoder && g_encoder->is_running()) {
        if (g_meter_in)
            meter_widget_update(g_meter_in, g_encoder->get_input_level());
        if (g_meter_out)
            meter_widget_update(g_meter_out, g_encoder->get_output_level());

        /* update spectrum and waterfall with TX output FFT */
        {
            float spec[RadaeEncoder::SPECTRUM_BINS];
            g_encoder->get_spectrum(spec, RadaeEncoder::SPECTRUM_BINS);
            if (g_spectrum)
                spectrum_widget_update(g_spectrum, spec, RadaeEncoder::SPECTRUM_BINS,
                                       g_encoder->spectrum_sample_rate());
            if (g_waterfall)
                waterfall_widget_update(g_waterfall, spec, RadaeEncoder::SPECTRUM_BINS,
                                        g_encoder->spectrum_sample_rate());
        }

        set_status("Transmitting\xe2\x80\xa6");
        return TRUE;
    }

    /* ── RX mode ─────────────────────────────────────────────────────── */
    if (!g_decoder) return TRUE;
    std::string cs = g_decoder->last_callsign();
    char buf[256];
    if (!g_decoder->is_running()) {
        /* decoder stopped itself (e.g. file playback finished) */
        stop_all();
        std::snprintf(buf, sizeof buf,
                          "Playback finished. %s", cs.c_str());
        set_status(buf);
        return FALSE;
    }

    /* update level meters */
    if (g_meter_in)
        meter_widget_update(g_meter_in, g_decoder->get_input_level());
    if (g_meter_out)
        meter_widget_update(g_meter_out, g_decoder->get_output_level_left());

    /* update spectrum and waterfall with input audio FFT */
    {
        float spec[RadaeDecoder::SPECTRUM_BINS];
        g_decoder->get_spectrum(spec, RadaeDecoder::SPECTRUM_BINS);
        if (g_spectrum)
            spectrum_widget_update(g_spectrum, spec, RadaeDecoder::SPECTRUM_BINS,
                                   g_decoder->spectrum_sample_rate());
        if (g_waterfall)
            waterfall_widget_update(g_waterfall, spec, RadaeDecoder::SPECTRUM_BINS,
                                    g_decoder->spectrum_sample_rate());
    }

    /* update status with sync info */
    
    if (g_decoder->is_synced()) {
        
        if (cs.empty()) {
            std::snprintf(buf, sizeof buf,
                          "Synced \xe2\x80\x94 SNR: %.0f dB  Freq: %+.1f Hz",
                          static_cast<double>(g_decoder->snr_dB()),
                          static_cast<double>(g_decoder->freq_offset()));
        } else {
            std::snprintf(buf, sizeof buf,
                          "Synced \xe2\x80\x94 SNR: %.0f dB  Freq: %+.1f Hz  Last call: %s",
                          static_cast<double>(g_decoder->snr_dB()),
                          static_cast<double>(g_decoder->freq_offset()),
                          cs.c_str());
        }
        set_status(buf);
    } else {
        if (cs.empty()) {
            std::snprintf(buf, sizeof buf,
                          "Searching for signal\xe2\x80\xa6");
        } else {
            std::snprintf(buf, sizeof buf,
                          "Searching for signal\xe2\x80\xa6 Last call: %s", cs.c_str());
        }
        set_status(buf);
    }

    return TRUE;
}

static void start_decoder(int in_idx, int out_idx)
{
    if (in_idx  < 0 || in_idx  >= static_cast<int>(g_input_devices.size()))  return;
    if (out_idx < 0 || out_idx >= static_cast<int>(g_output_devices.size())) return;

    stop_all();

    if (!g_decoder) g_decoder = new RadaeDecoder();

    if (!g_decoder->open(g_input_devices[in_idx].hw_id,
                         g_output_devices[out_idx].hw_id)) {
        set_status("Failed to open audio devices.");
        set_btn_state(false);
        return;
    }

    g_decoder->start();
    if (g_recording && g_recorder)
        g_decoder->set_recorder(g_recorder);
    set_btn_state(true);
    set_status("Searching for signal\xe2\x80\xa6");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);   // ~30 fps
}

static void start_encoder(int mic_idx, int radio_idx)
{
    if (mic_idx   < 0 || mic_idx   >= static_cast<int>(g_tx_input_devices.size()))  return;
    if (radio_idx < 0 || radio_idx >= static_cast<int>(g_tx_output_devices.size())) return;

    stop_all();

    if (!g_encoder) g_encoder = new RadaeEncoder();

    if (!g_encoder->open(g_tx_input_devices[mic_idx].hw_id,
                         g_tx_output_devices[radio_idx].hw_id)) {
        set_status("Failed to open TX audio devices.");
        set_btn_state(false);
        return;
    }

    if (g_bpf_switch)
        g_encoder->set_bpf_enabled(gtk_switch_get_active(GTK_SWITCH(g_bpf_switch)));
    if (g_callsign_entry) {
        const char* cs = gtk_entry_get_text(GTK_ENTRY(g_callsign_entry));
        g_encoder->set_callsign(cs ? cs : "");
    }
    g_encoder->start();
    if (g_recording && g_recorder)
        g_encoder->set_recorder(g_recorder);
    set_btn_state(true);
    set_status("Transmitting\xe2\x80\xa6");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);
}

/* ── signal handlers ────────────────────────────────────────────────────── */

/* selecting an input device: auto-start if output is also selected */
static void on_input_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int in_idx  = gtk_combo_box_get_active(combo);
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
    save_config();
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* selecting an output device: auto-start if input is also selected */
static void on_output_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int out_idx = gtk_combo_box_get_active(combo);
    int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    save_config();
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* selecting a transmit device: just save config */
static void on_tx_combo_changed(GtkComboBox* /*combo*/, gpointer /*data*/)
{
    if (g_updating_combos) return;
    save_config();
}

/* callsign entry changed: save config and update running encoder */
static void on_callsign_changed(GtkEditable* /*e*/, gpointer /*data*/)
{
    save_config();
    if (g_encoder && g_callsign_entry) {
        const char* cs = gtk_entry_get_text(GTK_ENTRY(g_callsign_entry));
        g_encoder->set_callsign(cs ? cs : "");
    }
}

/* gridsquare entry changed: save config */
static void on_gridsquare_changed(GtkEditable* /*e*/, gpointer /*data*/)
{
    save_config();
}

/* TX switch toggled: stop current mode and start the new one */
static gboolean on_tx_switch_changed(GtkSwitch* sw, gboolean state, gpointer /*data*/)
{
    gtk_switch_set_state(sw, state);

    bool was_running = (g_decoder && g_decoder->is_running()) ||
                       (g_encoder && g_encoder->is_running());
    if (!was_running) return TRUE;

    stop_all();

    if (state) {
        /* switched to TX */
        int mic_idx   = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_input_combo));
        int radio_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_output_combo));
        if (mic_idx >= 0 && radio_idx >= 0)
            start_encoder(mic_idx, radio_idx);
        else
            set_status("Select Microphone In and Radio Out in Settings first.");
    } else {
        /* switched to RX */
        int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
        int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
        if (in_idx >= 0 && out_idx >= 0)
            start_decoder(in_idx, out_idx);
        else
            set_status("Select both input and output devices first.");
    }

    return TRUE;
}

/* BPF switch toggled */
static gboolean on_bpf_switch_changed(GtkSwitch* sw, gboolean state, gpointer /*data*/)
{
    gtk_switch_set_state(sw, state);
    if (g_encoder)
        g_encoder->set_bpf_enabled(state);
    return TRUE;
}

/* record button: start/stop WAV recording */
static void on_record_clicked(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (!g_recording) {
        /* ── start recording ── */
        if (!g_recorder) g_recorder = new WavRecorder();

        /* delete any existing recording.wav before opening */
        std::remove("recording.wav");

        if (!g_recorder->open("recording.wav")) {
            set_status("Failed to create recording.wav");
            delete g_recorder;
            g_recorder = nullptr;
            return;
        }

        g_recording = true;

        /* update button appearance */
        GtkStyleContext* ctx = gtk_widget_get_style_context(g_record_btn);
        gtk_style_context_remove_class(ctx, "record-btn");
        gtk_style_context_add_class   (ctx, "record-stop-btn");
        gtk_button_set_label(GTK_BUTTON(g_record_btn), "Stop");

        /* attach to whichever pipeline is currently running */
        if (g_decoder) g_decoder->set_recorder(g_recorder);
        if (g_encoder) g_encoder->set_recorder(g_recorder);

    } else {
        /* ── stop recording ── */

        /* detach from pipelines first (waits for any in-progress write) */
        if (g_decoder) g_decoder->set_recorder(nullptr);
        if (g_encoder) g_encoder->set_recorder(nullptr);

        if (g_recorder) {
            g_recorder->close();
            delete g_recorder;
            g_recorder = nullptr;
        }

        g_recording = false;

        /* restore button appearance */
        GtkStyleContext* ctx = gtk_widget_get_style_context(g_record_btn);
        gtk_style_context_remove_class(ctx, "record-stop-btn");
        gtk_style_context_add_class   (ctx, "record-btn");
        gtk_button_set_label(GTK_BUTTON(g_record_btn), " Record ");
    }
}

/* start / stop toggle */
static void on_start_stop(GtkButton* /*btn*/, gpointer /*data*/)
{
    bool running = (g_decoder && g_decoder->is_running()) ||
                   (g_encoder && g_encoder->is_running());
    if (running) {
        stop_all();
        set_status("Stopped.");
        return;
    }

    bool tx_mode = g_tx_switch && gtk_switch_get_active(GTK_SWITCH(g_tx_switch));

    if (tx_mode) {
        int mic_idx   = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_input_combo));
        int radio_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_output_combo));
        if (mic_idx < 0 || radio_idx < 0) {
            set_status("Select Microphone In and Radio Out in Settings first.");
            return;
        }
        start_encoder(mic_idx, radio_idx);
    } else {
        int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
        int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
        if (in_idx < 0 || out_idx < 0) {
            set_status("Select both input and output devices first.");
            return;
        }
        start_decoder(in_idx, out_idx);
    }
}

/* refresh the device lists */
static void on_refresh(GtkButton* /*btn*/, gpointer /*data*/)
{
    bool running = (g_decoder && g_decoder->is_running()) ||
                   (g_encoder && g_encoder->is_running());
    if (running) stop_all();

    g_input_devices    = AudioInput::enumerate_devices();
    g_output_devices   = AudioInput::enumerate_playback_devices();
    g_tx_input_devices = AudioInput::enumerate_devices();
    g_tx_output_devices = AudioInput::enumerate_playback_devices();

    g_updating_combos = true;

    /* populate input combo */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_input_combo));
    for (const auto& d : g_input_devices)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_input_combo), d.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_input_combo), -1);

    /* populate output combo */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_output_combo));
    for (const auto& d : g_output_devices)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_output_combo), d.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_output_combo), -1);

    /* populate transmit input combo (mic) */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_tx_input_combo));
    for (const auto& d : g_tx_input_devices)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_tx_input_combo), d.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_input_combo), -1);

    /* populate transmit output combo (radio) */
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(g_tx_output_combo));
    for (const auto& d : g_tx_output_devices)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_tx_output_combo), d.name.c_str());
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_output_combo), -1);

    g_updating_combos = false;

    set_status(g_input_devices.empty()
               ? "No audio input devices found."
               : "Select input and output devices above.");
}

/* clean up before the window disappears */
static void on_window_destroy(GtkWidget* /*w*/, gpointer /*data*/)
{
    save_config();
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    /* detach recorder before stopping threads */
    if (g_decoder) g_decoder->set_recorder(nullptr);
    if (g_encoder) g_encoder->set_recorder(nullptr);
    if (g_decoder) { g_decoder->stop(); g_decoder->close(); delete g_decoder; g_decoder = nullptr; }
    if (g_encoder) { g_encoder->stop(); g_encoder->close(); delete g_encoder; g_encoder = nullptr; }
    /* finalise any open recording */
    if (g_recorder) { g_recorder->close(); delete g_recorder; g_recorder = nullptr; }
}

/* ── file playback ─────────────────────────────────────────────────────── */

static void start_decoder_file(const std::string& wav_path, int out_idx)
{
    if (out_idx < 0 || out_idx >= static_cast<int>(g_output_devices.size())) return;

    stop_all();

    if (!g_decoder) g_decoder = new RadaeDecoder();

    if (!g_decoder->open_file(wav_path,
                               g_output_devices[static_cast<size_t>(out_idx)].hw_id)) {
        set_status("Failed to open WAV file or audio output.");
        set_btn_state(false);
        return;
    }

    g_decoder->start();
    set_btn_state(true);
    set_status("Playing file\xe2\x80\xa6");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);
}

/* File > Open */
static void on_open_file(GtkMenuItem* /*item*/, gpointer parent_window)
{
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
    if (out_idx < 0) {
        set_status("Select an output device first (Edit > Settings).");
        return;
    }

    GtkWidget* dialog = gtk_file_chooser_dialog_new(
        "Open WAV File",
        GTK_WINDOW(parent_window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        nullptr);

    GtkFileFilter* filter_wav = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_wav, "WAV files (*.wav)");
    gtk_file_filter_add_pattern(filter_wav, "*.wav");
    gtk_file_filter_add_pattern(filter_wav, "*.WAV");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_wav);

    GtkFileFilter* filter_all = gtk_file_filter_new();
    gtk_file_filter_set_name(filter_all, "All files");
    gtk_file_filter_add_pattern(filter_all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_all);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            start_decoder_file(filename, out_idx);
            g_free(filename);
        }
    }

    gtk_widget_destroy(dialog);
}

/* File > Quit */
static void on_quit(GtkMenuItem* /*item*/, gpointer app)
{
    g_application_quit(G_APPLICATION(app));
}

/* Edit > Settings */
static void on_settings(GtkMenuItem* /*item*/, gpointer /*data*/)
{
    if (g_settings_dlg)
        gtk_widget_show_all(g_settings_dlg);
}

/* ── UI construction ────────────────────────────────────────────────────── */

static void activate(GtkApplication* app, gpointer /*data*/)
{
    /* ── CSS ───────────────────────────────────────────────────────── */
    GtkCssProvider* css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, R"CSS(
        button.start-btn {
            background-color  : #27ae60;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.start-btn:hover { background-color: #2ecc71; }

        button.stop-btn  {
            background-color  : #c0392b;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.stop-btn:hover  { background-color: #e74c3c; }

        button.record-btn {
            background-color  : #2980b9;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.record-btn:hover { background-color: #3498db; }

        button.record-stop-btn {
            background-color  : #8e44ad;
            color             : white;
            font-weight       : bold;
            border-radius     : 4px;
            padding           : 3px 0;
        }
        button.record-stop-btn:hover { background-color: #9b59b6; }

        #status-label { color: #888; }
    )CSS", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── window ────────────────────────────────────────────────────── */
    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title         (GTK_WINDOW(window), "RADAE GUI");
    gtk_window_set_default_size  (GTK_WINDOW(window), 500, 400);
    gtk_window_set_resizable     (GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    /* ── menu bar ──────────────────────────────────────────────────── */
    GtkWidget* outer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), outer_vbox);

    GtkAccelGroup *accel_group = gtk_accel_group_new();
    gtk_window_add_accel_group(GTK_WINDOW(window), accel_group);

    GtkWidget* menubar  = gtk_menu_bar_new();
    GtkWidget* file_mi  = gtk_menu_item_new_with_label("File");
    GtkWidget* file_menu = gtk_menu_new();

    GtkWidget* open_mi  = gtk_menu_item_new_with_label("Open WAV\xe2\x80\xa6");
    g_signal_connect(open_mi, "activate", G_CALLBACK(on_open_file), window);
    gtk_widget_add_accelerator(open_mi, "activate", accel_group,
                               GDK_KEY_o, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_mi);

    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());

    GtkWidget* quit_mi  = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_mi, "activate", G_CALLBACK(on_quit), app);
    gtk_widget_add_accelerator(quit_mi, "activate", accel_group,
                               GDK_KEY_q, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_mi);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_mi), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_mi);

    GtkWidget* edit_mi   = gtk_menu_item_new_with_label("Edit");
    GtkWidget* edit_menu = gtk_menu_new();
    GtkWidget* settings_mi = gtk_menu_item_new_with_label("Settings\xe2\x80\xa6");
    g_signal_connect(settings_mi, "activate", G_CALLBACK(on_settings), window);
    gtk_widget_add_accelerator(settings_mi, "activate", accel_group,
                               GDK_KEY_comma, GDK_CONTROL_MASK, GTK_ACCEL_VISIBLE);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), settings_mi);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_mi), edit_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_mi);

    gtk_box_pack_start(GTK_BOX(outer_vbox), menubar, FALSE, FALSE, 0);

    /* ── settings dialog (created hidden, shown from Edit > Settings) ── */
    g_settings_dlg = gtk_dialog_new_with_buttons(
        "Settings",
        GTK_WINDOW(window),
        GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE,
        nullptr);
    gtk_window_set_default_size(GTK_WINDOW(g_settings_dlg), 400, -1);
    g_signal_connect(g_settings_dlg, "delete-event",
                     G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
    g_signal_connect_swapped(g_settings_dlg, "response",
                             G_CALLBACK(gtk_widget_hide), g_settings_dlg);

    GtkWidget* scontent = gtk_dialog_get_content_area(GTK_DIALOG(g_settings_dlg));
    gtk_container_set_border_width(GTK_CONTAINER(scontent), 12);
    gtk_box_set_spacing(GTK_BOX(scontent), 8);

    GtkWidget* rx_heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(rx_heading), "<b>Receive</b>");
    gtk_label_set_xalign(GTK_LABEL(rx_heading), 0.0);
    gtk_box_pack_start(GTK_BOX(scontent), rx_heading, FALSE, FALSE, 0);

    /* ── input device selector row ────────────────────────────────── */
    GtkWidget* input_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* input_label = gtk_label_new("Input from radio:");
    gtk_widget_set_size_request(input_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(input_label), 0.0);
    gtk_box_pack_start(GTK_BOX(input_hbox), input_label, FALSE, FALSE, 0);

    g_input_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(g_input_combo, "Audio input (RADAE modem signal)");
    g_signal_connect(g_input_combo, "changed", G_CALLBACK(on_input_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(input_hbox), g_input_combo, TRUE, TRUE, 0);

    GtkWidget* refresh = gtk_button_new_with_label("\xe2\x86\xbb");   // ↻ UTF-8
    gtk_widget_set_tooltip_text(refresh, "Refresh device lists");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    gtk_box_pack_start(GTK_BOX(input_hbox), refresh, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), input_hbox, FALSE, FALSE, 0);

    /* ── output device selector row ────────────────────────────────── */
    GtkWidget* output_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* output_label = gtk_label_new("Output to speaker:");
    gtk_widget_set_size_request(output_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(output_label), 0.0);
    gtk_box_pack_start(GTK_BOX(output_hbox), output_label, FALSE, FALSE, 0);

    g_output_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(g_output_combo, "Audio output (decoded speech)");
    g_signal_connect(g_output_combo, "changed", G_CALLBACK(on_output_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(output_hbox), g_output_combo, TRUE, TRUE, 0);

    /* spacer to align with refresh button above */
    GtkWidget* spacer = gtk_label_new("");
    gtk_widget_set_size_request(spacer, 28, -1);
    gtk_box_pack_start(GTK_BOX(output_hbox), spacer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), output_hbox, FALSE, FALSE, 0);

    /* ── separator between Receive and Transmit sections ──────────── */
    gtk_box_pack_start(GTK_BOX(scontent),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget* tx_heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(tx_heading), "<b>Transmit</b>");
    gtk_label_set_xalign(GTK_LABEL(tx_heading), 0.0);
    gtk_box_pack_start(GTK_BOX(scontent), tx_heading, FALSE, FALSE, 0);

    /* ── transmit input (microphone) selector row ─────────────────── */
    GtkWidget* tx_input_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* tx_input_label = gtk_label_new("Microphone In:");
    gtk_widget_set_size_request(tx_input_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(tx_input_label), 0.0);
    gtk_box_pack_start(GTK_BOX(tx_input_hbox), tx_input_label, FALSE, FALSE, 0);

    g_tx_input_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(g_tx_input_combo, "Microphone input for transmit");
    g_signal_connect(g_tx_input_combo, "changed", G_CALLBACK(on_tx_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(tx_input_hbox), g_tx_input_combo, TRUE, TRUE, 0);

    /* spacer to align with refresh button above */
    GtkWidget* tx_in_spacer = gtk_label_new("");
    gtk_widget_set_size_request(tx_in_spacer, 28, -1);
    gtk_box_pack_start(GTK_BOX(tx_input_hbox), tx_in_spacer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), tx_input_hbox, FALSE, FALSE, 0);

    /* ── transmit output (radio) selector row ─────────────────────── */
    GtkWidget* tx_output_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* tx_output_label = gtk_label_new("Output to Radio:");
    gtk_widget_set_size_request(tx_output_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(tx_output_label), 0.0);
    gtk_box_pack_start(GTK_BOX(tx_output_hbox), tx_output_label, FALSE, FALSE, 0);

    g_tx_output_combo = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(g_tx_output_combo, "Audio output to radio for transmit");
    g_signal_connect(g_tx_output_combo, "changed", G_CALLBACK(on_tx_combo_changed), NULL);
    gtk_box_pack_start(GTK_BOX(tx_output_hbox), g_tx_output_combo, TRUE, TRUE, 0);

    /* spacer to align with refresh button above */
    GtkWidget* tx_out_spacer = gtk_label_new("");
    gtk_widget_set_size_request(tx_out_spacer, 28, -1);
    gtk_box_pack_start(GTK_BOX(tx_output_hbox), tx_out_spacer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), tx_output_hbox, FALSE, FALSE, 0);

    /* ── separator between Transmit and Station sections ──────────── */
    gtk_box_pack_start(GTK_BOX(scontent),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

    GtkWidget* station_heading = gtk_label_new(nullptr);
    gtk_label_set_markup(GTK_LABEL(station_heading), "<b>Station</b>");
    gtk_label_set_xalign(GTK_LABEL(station_heading), 0.0);
    gtk_box_pack_start(GTK_BOX(scontent), station_heading, FALSE, FALSE, 0);

    /* ── callsign entry row ───────────────────────────────────────── */
    GtkWidget* callsign_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* callsign_label = gtk_label_new("Callsign:");
    gtk_widget_set_size_request(callsign_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(callsign_label), 0.0);
    gtk_box_pack_start(GTK_BOX(callsign_hbox), callsign_label, FALSE, FALSE, 0);

    g_callsign_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_callsign_entry), "e.g. VK2XYZ");
    gtk_entry_set_max_length(GTK_ENTRY(g_callsign_entry), 8);
    gtk_widget_set_tooltip_text(g_callsign_entry, "Your station callsign (sent in end-of-over packet)");
    g_signal_connect(g_callsign_entry, "changed", G_CALLBACK(on_callsign_changed), NULL);
    gtk_box_pack_start(GTK_BOX(callsign_hbox), g_callsign_entry, TRUE, TRUE, 0);

    /* spacer to align with refresh button above */
    GtkWidget* cs_spacer = gtk_label_new("");
    gtk_widget_set_size_request(cs_spacer, 28, -1);
    gtk_box_pack_start(GTK_BOX(callsign_hbox), cs_spacer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), callsign_hbox, FALSE, FALSE, 0);

    /* ── gridsquare entry row ─────────────────────────────────────── */
    GtkWidget* gridsquare_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* gridsquare_label = gtk_label_new("Grid Square:");
    gtk_widget_set_size_request(gridsquare_label, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(gridsquare_label), 0.0);
    gtk_box_pack_start(GTK_BOX(gridsquare_hbox), gridsquare_label, FALSE, FALSE, 0);

    g_gridsquare_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_gridsquare_entry), "e.g. QF56");
    gtk_entry_set_max_length(GTK_ENTRY(g_gridsquare_entry), 8);
    gtk_widget_set_tooltip_text(g_gridsquare_entry, "Your Maidenhead grid square locator");
    g_signal_connect(g_gridsquare_entry, "changed", G_CALLBACK(on_gridsquare_changed), NULL);
    gtk_box_pack_start(GTK_BOX(gridsquare_hbox), g_gridsquare_entry, TRUE, TRUE, 0);

    /* spacer to align with refresh button above */
    GtkWidget* gs_spacer = gtk_label_new("");
    gtk_widget_set_size_request(gs_spacer, 28, -1);
    gtk_box_pack_start(GTK_BOX(gridsquare_hbox), gs_spacer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), gridsquare_hbox, FALSE, FALSE, 0);

    /* ── layout ────────────────────────────────────────────────────── */
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_box_pack_start(GTK_BOX(outer_vbox), vbox, TRUE, TRUE, 0);

    /* ── start / stop button + TX switch (side by side) ──────────── */
    GtkWidget* btn_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    g_btn = gtk_button_new_with_label("Start");
    gtk_widget_get_style_context(g_btn);                        // ensure context exists
    gtk_style_context_add_class(gtk_widget_get_style_context(g_btn), "start-btn");
    g_signal_connect(g_btn, "clicked", G_CALLBACK(on_start_stop), NULL);
    gtk_box_pack_start(GTK_BOX(btn_hbox), g_btn, TRUE, TRUE, 0);

    g_record_btn = gtk_button_new_with_label(" Record ");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_record_btn), "record-btn");
    gtk_widget_set_tooltip_text(g_record_btn, "Record radio audio to recording.wav");
    g_signal_connect(g_record_btn, "clicked", G_CALLBACK(on_record_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btn_hbox), g_record_btn, FALSE, FALSE, 0);

    GtkWidget* tx_label = gtk_label_new("TX");
    gtk_box_pack_start(GTK_BOX(btn_hbox), tx_label, FALSE, FALSE, 0);

    g_tx_switch = gtk_switch_new();
    gtk_widget_set_tooltip_text(g_tx_switch, "Toggle transmit mode");
    gtk_widget_set_valign(g_tx_switch, GTK_ALIGN_CENTER);
    g_signal_connect(g_tx_switch, "state-set", G_CALLBACK(on_tx_switch_changed), NULL);
    gtk_box_pack_start(GTK_BOX(btn_hbox), g_tx_switch, FALSE, FALSE, 0);

    /* spacer between TX and BPF switches */
    gtk_box_pack_start(GTK_BOX(btn_hbox), gtk_label_new(""), FALSE, FALSE, 4);

    GtkWidget* bpf_label = gtk_label_new("BPF");
    gtk_box_pack_start(GTK_BOX(btn_hbox), bpf_label, FALSE, FALSE, 0);

    g_bpf_switch = gtk_switch_new();
    gtk_widget_set_tooltip_text(g_bpf_switch, "700\xe2\x80\x93" "2300 Hz bandpass filter on TX output");
    gtk_widget_set_valign(g_bpf_switch, GTK_ALIGN_CENTER);
    g_signal_connect(g_bpf_switch, "state-set", G_CALLBACK(on_bpf_switch_changed), NULL);
    gtk_box_pack_start(GTK_BOX(btn_hbox), g_bpf_switch, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), btn_hbox, FALSE, FALSE, 0);

    /* ── input meter + spectrum + output meter (side by side) ────────── */
    GtkWidget* meter_spec_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    g_mic_slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    gtk_range_set_inverted(GTK_RANGE(g_mic_slider), TRUE);   /* 100 at top */
    gtk_range_set_value(GTK_RANGE(g_mic_slider), 50);
    gtk_scale_set_draw_value(GTK_SCALE(g_mic_slider), FALSE);
    gtk_widget_set_size_request(g_mic_slider, 30, -1);
    gtk_widget_set_tooltip_text(g_mic_slider, "TX mic input level");
    g_signal_connect(g_mic_slider, "value-changed",
                     G_CALLBACK(on_mic_level_changed), NULL);
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_mic_slider, FALSE, FALSE, 0);

    g_meter_in = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_meter_in, FALSE, FALSE, 0);

    GtkWidget* spec_waterfall_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    g_spectrum = spectrum_widget_new();
    gtk_box_pack_start(GTK_BOX(spec_waterfall_vbox), g_spectrum, TRUE, TRUE, 0);

    g_waterfall = waterfall_widget_new();
    gtk_box_pack_start(GTK_BOX(spec_waterfall_vbox), g_waterfall, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), spec_waterfall_vbox, TRUE, TRUE, 0);

    g_meter_out = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_meter_out, FALSE, FALSE, 0);

    g_tx_slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    gtk_range_set_inverted(GTK_RANGE(g_tx_slider), TRUE);   /* 100 at top */
    gtk_range_set_value(GTK_RANGE(g_tx_slider), 50);
    gtk_scale_set_draw_value(GTK_SCALE(g_tx_slider), FALSE);
    gtk_widget_set_size_request(g_tx_slider, 30, -1);
    gtk_widget_set_tooltip_text(g_tx_slider, "TX output level");
    g_signal_connect(g_tx_slider, "value-changed",
                     G_CALLBACK(on_tx_level_changed), NULL);
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_tx_slider, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), meter_spec_hbox, TRUE, TRUE, 0);

    /* ── status label ──────────────────────────────────────────────── */
    g_status = gtk_label_new("");
    gtk_widget_set_name(g_status, "status-label");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    /* ── show everything, then populate the combo ──────────────────── */
    gtk_widget_show_all(window);
    on_refresh(nullptr, nullptr);                  // first device-list load

    /* ── restore saved device selections ──────────────────────────── */
    if (restore_config()) {
        int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
        int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
        if (in_idx >= 0 && out_idx >= 0)
            start_decoder(in_idx, out_idx);
    }
}

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
