#include "gui_controls.h"
#include "gui_app_state.h"
#include "gui_callbacks.h"    // on_meter_tick — passed to g_timeout_add
#include "meter_widget.h"
#include "spectrum_widget.h"
#include "waterfall_widget.h"
#include "rig_control.h"

#include <cstdio>
#include <cstring>

/* ── helpers ────────────────────────────────────────────────────────────── */

void set_status(const char* msg)
{
    gtk_label_set_text(GTK_LABEL(g_status), msg);
}

/* change the button label AND its CSS class in one shot */
void set_btn_state(bool capturing)
{
    //GtkStyleContext* ctx = gtk_widget_get_style_context(g_btn);
    if (capturing) {
        //gtk_style_context_remove_class(ctx, "start-btn");
        //gtk_style_context_add_class   (ctx, "stop-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Stop");
    } else {
        //gtk_style_context_remove_class(ctx, "stop-btn");
        //gtk_style_context_add_class   (ctx, "start-btn");
        gtk_button_set_label(GTK_BUTTON(g_btn), "Start");
    }
}

/* ── TX level slider callbacks ─────────────────────────────────────────── */

void on_mic_level_changed(GtkRange* range, gpointer /*data*/)
{
    double pct = gtk_range_get_value(range);
    float gain = static_cast<float>(pct / 100.0 * 2.0);   /* 0-100% → 0.0-2.0 */
    if (g_encoder)
        g_encoder->set_mic_gain(gain);
}

void on_tx_level_changed(GtkRange* range, gpointer /*data*/)
{
    double pct = gtk_range_get_value(range);
    float scale = static_cast<float>(pct / 100.0 * 32767.0);
    if (g_encoder)
        g_encoder->set_tx_scale(scale);
}

/* ── rig helpers ────────────────────────────────────────────────────────── */

void update_rig_status_label()
{
    if (!g_rig_status_lbl) return;
    char buf[160];
    if (!rig_is_connected()) {
        std::snprintf(buf, sizeof buf, "Rig: not connected");
    } else {
        const std::string freq = rig_get_current_freq();
        const std::string mode = rig_get_current_mode();
        const char* ptt = rig_get_ptt_on() ? "TX" : "RX";
        std::snprintf(buf, sizeof buf, "Rig: %s  |  %s  |  %s",
                      ptt,
                      freq.empty() ? "--" : freq.c_str(),
                      mode.empty() ? "--" : mode.c_str());
    }
    gtk_label_set_text(GTK_LABEL(g_rig_status_lbl), buf);
}

/* Parse a rig frequency string (e.g. "14.225.000 MHz") to whole Hz.
   Returns 0 when no rig is connected or the string is empty. */
uint64_t rig_freq_hz()
{
    if (!rig_is_connected()) return 0;
    const std::string s = rig_get_current_freq();
    std::string digits;
    for (char c : s)
        if (c >= '0' && c <= '9') digits += c;
    return digits.empty() ? 0 : std::stoull(digits);
}

/* ── reporter ───────────────────────────────────────────────────────────── */

/* (Re)create the FreeDV Reporter with the current callsign and grid square.
   Safe to call at any time; deletes and replaces any existing instance.
   Only called after the settings widgets exist (end of activate() or later). */
void reporter_restart()
{
    fprintf(stderr, "reporter_restart()\n");

    delete g_reporter;
    g_reporter = nullptr;

    const char* cs = g_callsign_entry
                   ? gtk_entry_get_text(GTK_ENTRY(g_callsign_entry)) : "";
    const char* gs = g_gridsquare_entry
                   ? gtk_entry_get_text(GTK_ENTRY(g_gridsquare_entry)) : "";

    g_reporter = new FreeDVReporter(cs ? cs : "", gs ? gs : "", "RADAEV1c"); // "FreeDV 2.2.1"
    g_reporter->connect();

    /* Re-send the saved free-text message so the reporter shows it immediately
       after (re)connecting, without the user having to press Send again. */
    if (g_message_entry) {
        const char* msg = gtk_entry_get_text(GTK_ENTRY(g_message_entry));
        if (msg && msg[0] != '\0')
            g_reporter->updateMessage(msg);
    }

    g_last_rx_callsign.clear();
    g_last_reporter_freq = 0;
}

/* ── decoder / encoder lifecycle ────────────────────────────────────────── */

void stop_all()
{
    /* Capture TX state before threads are stopped. */
    const bool was_transmitting = g_encoder && g_encoder->is_running();

    fprintf(stderr, "stop_all()\n");
    /* Stop threads first so the EOO frame is flushed into the recorder,
       then detach the recorder once the threads have finished. */
    if (g_decoder)    { g_decoder->stop();    g_decoder->close();    }
    if (g_encoder)    { g_encoder->stop();    g_encoder->close();    }
    if (g_passthrough){ g_passthrough->stop(); g_passthrough->close(); }

    if (g_decoder) g_decoder->set_recorder(nullptr);
    if (g_encoder) g_encoder->set_recorder(nullptr);
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    if (g_meter_in)  meter_widget_update(g_meter_in, 0.f);
    if (g_meter_out) meter_widget_update(g_meter_out, 0.f);
    if (g_spectrum)  spectrum_widget_update(g_spectrum, nullptr, 0, 8000.f);
    if (g_waterfall) waterfall_widget_update(g_waterfall, nullptr, 0, 8000.f);
    set_btn_state(false);
    rig_control_set_ptt(false);       /* release PTT if rig is connected */
    update_rig_status_label();        /* refresh immediately; timer is now stopped */

    /* Tell the reporter we stopped transmitting. */
    if (g_reporter && was_transmitting)
        g_reporter->transmit("RADAEV1c", false);
}

void start_decoder(int in_idx, int out_idx)
{
    fprintf(stderr, "start_decoder()\n");
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

void start_encoder(int mic_idx, int radio_idx)
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
    rig_control_set_ptt(true);        /* key the rig if connected */
    if (g_reporter)
        g_reporter->transmit("RADAEV1c", true);
    if (g_recording && g_recorder)
        g_encoder->set_recorder(g_recorder);
    set_btn_state(true);
    if (rig_is_connected() && (!rig_is_ptt_supported() || !rig_get_ptt_on()))
        set_status("Transmitting audio\xe2\x80\xa6 CAT PTT unsupported (use VOX/external PTT).");
    else
        set_status("Transmitting\xe2\x80\xa6");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);
}

void start_decoder_file(const std::string& wav_path, int out_idx)
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

void start_passthrough(int in_idx, int out_idx)
{
    if (in_idx  < 0 || in_idx  >= static_cast<int>(g_input_devices.size()))  return;
    if (out_idx < 0 || out_idx >= static_cast<int>(g_output_devices.size())) return;

    stop_all();

    if (!g_passthrough) g_passthrough = new AudioPassthrough();

    if (!g_passthrough->open(g_input_devices[static_cast<size_t>(in_idx)].hw_id,
                              g_output_devices[static_cast<size_t>(out_idx)].hw_id)) {
        set_status("Failed to open audio devices for passthrough.");
        return;
    }

    g_passthrough->start();
    set_status("Analog passthrough active.");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);   // ~30 fps
}
