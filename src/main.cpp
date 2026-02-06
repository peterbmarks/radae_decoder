#include <gtk/gtk.h>
#include <vector>
#include <string>
#include <cstdio>

#include "audio_input.h"
#include "meter_widget.h"
#include "rade_decoder.h"
#include "spectrum_widget.h"

/* ── globals (single-window app) ────────────────────────────────────────── */

static RadaeDecoder*            g_decoder            = nullptr;
static std::vector<AudioDevice> g_input_devices;
static std::vector<AudioDevice> g_output_devices;
static GtkWidget*               g_input_combo        = nullptr;   // input device selector
static GtkWidget*               g_output_combo       = nullptr;   // output device selector
static GtkWidget*               g_btn                = nullptr;   // start / stop
static GtkWidget*               g_meter              = nullptr;   // bar-meter widget
static GtkWidget*               g_spectrum           = nullptr;   // spectrum widget
static GtkWidget*               g_status             = nullptr;   // status label
static guint                    g_timer              = 0;         // meter update timer
static bool                     g_updating_combos    = false;     // guard programmatic changes

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

/* ── decoder control ───────────────────────────────────────────────────── */

static void stop_decoder()
{
    if (g_decoder) { g_decoder->stop(); g_decoder->close(); }
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    if (g_meter)     meter_widget_update(g_meter, 0.f, 0.f);
    if (g_spectrum)  spectrum_widget_update(g_spectrum, nullptr, 0, 8000.f);
    set_btn_state(false);
}

/* timer tick – update meter + status at ~30 fps */
static gboolean on_meter_tick(gpointer /*data*/)
{
    if (!g_decoder || !g_decoder->is_running()) return TRUE;

    /* update meter with decoded output level */
    if (g_meter)
        meter_widget_update(g_meter,
                            g_decoder->get_output_level_left(),
                            g_decoder->get_output_level_right());

    /* update spectrum with input audio FFT */
    if (g_spectrum) {
        float spec[RadaeDecoder::SPECTRUM_BINS];
        g_decoder->get_spectrum(spec, RadaeDecoder::SPECTRUM_BINS);
        spectrum_widget_update(g_spectrum, spec, RadaeDecoder::SPECTRUM_BINS,
                               g_decoder->spectrum_sample_rate());
    }

    /* update status with sync info */
    if (g_decoder->is_synced()) {
        char buf[128];
        std::snprintf(buf, sizeof buf,
                      "Synced \xe2\x80\x94 SNR: %.0f dB  Freq: %+.1f Hz",
                      static_cast<double>(g_decoder->snr_dB()),
                      static_cast<double>(g_decoder->freq_offset()));
        set_status(buf);
    } else {
        set_status("Searching for signal\xe2\x80\xa6");
    }

    return TRUE;
}

static void start_decoder(int in_idx, int out_idx)
{
    if (in_idx  < 0 || in_idx  >= static_cast<int>(g_input_devices.size()))  return;
    if (out_idx < 0 || out_idx >= static_cast<int>(g_output_devices.size())) return;

    stop_decoder();

    if (!g_decoder) g_decoder = new RadaeDecoder();

    if (!g_decoder->open(g_input_devices[in_idx].hw_id,
                         g_output_devices[out_idx].hw_id)) {
        set_status("Failed to open audio devices.");
        set_btn_state(false);
        return;
    }

    g_decoder->start();
    set_btn_state(true);
    set_status("Searching for signal\xe2\x80\xa6");
    g_timer = g_timeout_add(33, on_meter_tick, nullptr);   // ~30 fps
}

/* ── signal handlers ────────────────────────────────────────────────────── */

/* selecting an input device: auto-start if output is also selected */
static void on_input_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int in_idx  = gtk_combo_box_get_active(combo);
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* selecting an output device: auto-start if input is also selected */
static void on_output_combo_changed(GtkComboBox* combo, gpointer /*data*/)
{
    if (g_updating_combos) return;
    int out_idx = gtk_combo_box_get_active(combo);
    int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    if (in_idx >= 0 && out_idx >= 0)
        start_decoder(in_idx, out_idx);
}

/* start / stop toggle */
static void on_start_stop(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (g_decoder && g_decoder->is_running()) {
        stop_decoder();
        set_status("Stopped.");
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
    if (g_decoder && g_decoder->is_running()) stop_decoder();

    g_input_devices  = AudioInput::enumerate_devices();
    g_output_devices = AudioInput::enumerate_playback_devices();

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

    g_updating_combos = false;

    set_status(g_input_devices.empty()
               ? "No audio input devices found."
               : "Select input and output devices above.");
}

/* clean up before the window disappears */
static void on_window_destroy(GtkWidget* /*w*/, gpointer /*data*/)
{
    if (g_timer)   { g_source_remove(g_timer); g_timer = 0; }
    if (g_decoder) { g_decoder->stop(); g_decoder->close(); delete g_decoder; g_decoder = nullptr; }
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

        #status-label { color: #888; }
    )CSS", -1, nullptr);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    /* ── window ────────────────────────────────────────────────────── */
    GtkWidget* window = gtk_application_window_new(app);
    gtk_window_set_title         (GTK_WINDOW(window), "RADAE Decoder");
    gtk_window_set_default_size  (GTK_WINDOW(window), 400, 580);
    gtk_window_set_resizable     (GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);

    /* ── layout ────────────────────────────────────────────────────── */
    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(window), vbox);

    /* ── input device selector row ────────────────────────────────── */
    GtkWidget* input_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* input_label = gtk_label_new("Input:");
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

    gtk_box_pack_start(GTK_BOX(vbox), input_hbox, FALSE, FALSE, 0);

    /* ── output device selector row ────────────────────────────────── */
    GtkWidget* output_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* output_label = gtk_label_new("Output:");
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

    gtk_box_pack_start(GTK_BOX(vbox), output_hbox, FALSE, FALSE, 0);

    /* ── start / stop button ───────────────────────────────────────── */
    g_btn = gtk_button_new_with_label("Start");
    gtk_widget_get_style_context(g_btn);                        // ensure context exists
    gtk_style_context_add_class(gtk_widget_get_style_context(g_btn), "start-btn");
    g_signal_connect(g_btn, "clicked", G_CALLBACK(on_start_stop), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_btn, FALSE, FALSE, 0);

    /* ── spectrum (input audio, fixed height) ──────────────────────── */
    g_spectrum = spectrum_widget_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_spectrum, FALSE, FALSE, 0);

    /* ── meter (expands to fill remaining vertical space) ─────────── */
    g_meter = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_meter, TRUE, TRUE, 0);

    /* ── status label ──────────────────────────────────────────────── */
    g_status = gtk_label_new("");
    gtk_widget_set_name(g_status, "status-label");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_status, FALSE, FALSE, 0);

    /* ── show everything, then populate the combo ──────────────────── */
    gtk_widget_show_all(window);
    on_refresh(nullptr, nullptr);                  // first device-list load
}

/* ── entry point ────────────────────────────────────────────────────────── */

int main(int argc, char* argv[])
{
    GtkApplication* app = gtk_application_new("org.simpledecoder.RADAEDecoder",
                                              G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int rc = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return rc;
}
