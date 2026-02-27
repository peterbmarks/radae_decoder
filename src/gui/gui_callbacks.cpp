#include "gui_callbacks.h"
#include "gui_app_state.h"
#include "gui_controls.h"
#include "gui_config.h"
#include "meter_widget.h"
#include "spectrum_widget.h"
#include "waterfall_widget.h"
#include "rig_control.h"

#include <algorithm>   // std::clamp
#include <cstdio>

/* ── timer callback: update meters + status at ~30 fps ─────────────────── */

gboolean on_meter_tick(gpointer /*data*/)
{
    update_rig_status_label();

    /* ── report frequency changes to FreeDV Reporter ──────────── */
    if (g_reporter) {
        const uint64_t cur_freq = rig_freq_hz();
        if (cur_freq != g_last_reporter_freq) {
            g_last_reporter_freq = cur_freq;
            if (cur_freq > 0)
                g_reporter->freqChange(cur_freq);
        }
    }

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

    /* ── analog passthrough mode ─────────────────────────────────────── */
    if (g_passthrough && g_passthrough->is_running()) {
        if (g_meter_in)
            meter_widget_update(g_meter_in, g_passthrough->get_input_level());
        if (g_meter_out)
            meter_widget_update(g_meter_out, 0.0f);

        float spec[AudioPassthrough::SPECTRUM_BINS];
        g_passthrough->get_spectrum(spec, AudioPassthrough::SPECTRUM_BINS);
        if (g_spectrum)
            spectrum_widget_update(g_spectrum, spec, AudioPassthrough::SPECTRUM_BINS,
                                   g_passthrough->spectrum_sample_rate());
        if (g_waterfall)
            waterfall_widget_update(g_waterfall, spec, AudioPassthrough::SPECTRUM_BINS,
                                    g_passthrough->spectrum_sample_rate());
        return TRUE;
    }

    /* ── RX mode ─────────────────────────────────────────────────────── */
    if (!g_decoder) return TRUE;
    std::string cs = g_decoder->last_callsign();

    /* Report newly decoded callsigns to FreeDV Reporter. */
    if (g_reporter && !cs.empty() && cs != g_last_rx_callsign) {
        g_last_rx_callsign = cs;
        const signed char snr = static_cast<signed char>(
            std::clamp(g_decoder->snr_dB(), -128.0f, 127.0f));
        g_reporter->addReceiveRecord(cs, "RADAEV1c", rig_freq_hz(), snr);
    }

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

/* ── device selection ───────────────────────────────────────────────────── */

/* selecting an input device: auto-start if output is also selected */
void on_input_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int in_idx  = gtk_combo_box_get_active(combo);
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
    save_config();
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* selecting an output device: auto-start if input is also selected */
void on_output_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int out_idx = gtk_combo_box_get_active(combo);
    int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    save_config();
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* selecting a transmit device: just save config */
void on_tx_combo_changed(GtkComboBox* /*combo*/, gpointer /*data*/)
{
    if (g_updating_combos) return;
    save_config();
}

/* ── station settings ───────────────────────────────────────────────────── */

/* callsign entry changed: save config, update running encoder, reconnect reporter */
void on_callsign_changed(GtkEditable* /*e*/, gpointer /*data*/)
{
    save_config();
    if (g_encoder && g_callsign_entry) {
        const char* cs = gtk_entry_get_text(GTK_ENTRY(g_callsign_entry));
        g_encoder->set_callsign(cs ? cs : "");
    }
    /* Only reconnect after the initial reporter has been created (end of activate). */
    if (g_reporter) reporter_restart();
}

/* gridsquare entry changed: save config and reconnect reporter */
void on_gridsquare_changed(GtkEditable* /*e*/, gpointer /*data*/)
{
    save_config();
    if (g_reporter) reporter_restart();
}

/* ── mode toggles ───────────────────────────────────────────────────────── */

/* TX switch toggled: stop current mode and start the new one */
gboolean on_tx_switch_changed(GtkSwitch* sw, gboolean state, gpointer /*data*/)
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
gboolean on_bpf_switch_changed(GtkSwitch* sw, gboolean state, gpointer /*data*/)
{
    gtk_switch_set_state(sw, state);
    if (g_encoder)
        g_encoder->set_bpf_enabled(state);
    return TRUE;
}

/* ── buttons ────────────────────────────────────────────────────────────── */

/* record button: start/stop WAV recording */
void on_record_clicked(GtkButton* /*btn*/, gpointer /*data*/)
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
void on_start_stop(GtkButton* /*btn*/, gpointer /*data*/)
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

/* analog passthrough toggle */
void on_analog_clicked(GtkButton* btn, gpointer /*data*/)
{
    if (!g_analog_active) {
        /* ── switch to analog passthrough ── */
        int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
        int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
        if (in_idx < 0 || out_idx < 0) {
            set_status("Select both input and output devices first.");
            return;
        }
        g_analog_active = true;
        gtk_button_set_label(btn, " Digital ");
        gtk_widget_set_tooltip_text(GTK_WIDGET(btn), "Switch back to RADE digital decoding");
        start_passthrough(in_idx, out_idx);
    } else {
        /* ── switch back to digital decode ── */
        g_analog_active = false;
        gtk_button_set_label(btn, " Analog ");
        gtk_widget_set_tooltip_text(GTK_WIDGET(btn), "Switch to analog passthrough (no decoding)");
        int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
        int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
        if (in_idx >= 0 && out_idx >= 0)
            start_decoder(in_idx, out_idx);
        else
            stop_all();
    }
}

/* refresh the device lists */
void on_refresh(GtkButton* /*btn*/, gpointer /*data*/)
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

/* ── window lifecycle ───────────────────────────────────────────────────── */

/* clean up before the window disappears */
void on_window_destroy(GtkWidget* /*w*/, gpointer /*data*/)
{
    save_config();
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    /* detach recorder before stopping threads */
    if (g_decoder) g_decoder->set_recorder(nullptr);
    if (g_encoder) g_encoder->set_recorder(nullptr);
    if (g_decoder) { g_decoder->stop(); g_decoder->close(); delete g_decoder; g_decoder = nullptr; }
    if (g_encoder) { g_encoder->stop(); g_encoder->close(); delete g_encoder; g_encoder = nullptr; }
    rig_control_cleanup();        /* release PTT, flush serial port, close rig */
    /* finalise any open recording */
    if (g_recorder) { g_recorder->close(); delete g_recorder; g_recorder = nullptr; }
}

/* ── file playback ─────────────────────────────────────────────────────── */

/* File > Open */
void on_open_file(GtkMenuItem* /*item*/, gpointer parent_window)
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

/* ── menu items ─────────────────────────────────────────────────────────── */

/* File > Quit */
void on_quit(GtkMenuItem* /*item*/, gpointer /*app*/)
{
    /* Destroying the window fires the "destroy" signal → on_window_destroy()
       which stops all threads cleanly before the app exits.  Using
       g_application_quit() directly bypasses that signal and leaves threads
       running, causing std::terminate() on their std::thread destructors. */
    if (g_window)
        gtk_widget_destroy(g_window);
}

/* Edit > Settings */
void on_settings(GtkMenuItem* /*item*/, gpointer /*data*/)
{
    if (g_settings_dlg)
        gtk_widget_show_all(g_settings_dlg);
}

/* Edit > Rig Control */
void on_rig_control(GtkMenuItem* /*item*/, gpointer /*data*/)
{
    if (g_rig_dlg)
        gtk_widget_show_all(g_rig_dlg);
}

/* Edit > Reporter */
void on_reporter(GtkMenuItem* /*item*/, gpointer /*data*/)
{
    gtk_show_uri_on_window(nullptr, "https://qso.freedv.org", GDK_CURRENT_TIME, nullptr);
}

/* Send reporter message */
void on_send_message(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (!g_reporter || !g_message_entry) return;
    const char* text = gtk_entry_get_text(GTK_ENTRY(g_message_entry));
    g_reporter->updateMessage(text ? text : "");
}
