#include "gui_activate.h"
#include "gui_app_state.h"
#include "gui_config.h"
#include "gui_controls.h"
#include "gui_callbacks.h"
#include "meter_widget.h"
#include "spectrum_widget.h"
#include "waterfall_widget.h"
#include "rig_control.h"

/* ── UI construction ────────────────────────────────────────────────────── */

void activate(GtkApplication* app, gpointer /*data*/)
{
    /* ── load hamlib backends (before any UI) ────────────────────── */
    rig_control_init();

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
    g_window = gtk_application_window_new(app);
    GtkWidget* window = g_window;
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

    GtkWidget* rig_mi = gtk_menu_item_new_with_label("Rig Control\xe2\x80\xa6");
    g_signal_connect(rig_mi, "activate", G_CALLBACK(on_rig_control), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), rig_mi);

    GtkWidget* reporter_mi = gtk_menu_item_new_with_label("Reporter");
    g_signal_connect(reporter_mi, "activate", G_CALLBACK(on_reporter), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu), reporter_mi);

    gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_mi), edit_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_mi);

    gtk_box_pack_start(GTK_BOX(outer_vbox), menubar, FALSE, FALSE, 0);

    /* ── rig control dialog (created hidden, shown from Edit > Rig Control) ── */
    g_rig_dlg = rig_control_create_dialog(window);
    rig_control_set_save_callback(save_config);

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

    /* ── BPF toggle row ─────────────────────────────────────────────── */
    GtkWidget* bpf_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget* bpf_lbl = gtk_label_new("BPF:");
    gtk_widget_set_size_request(bpf_lbl, 50, -1);
    gtk_label_set_xalign(GTK_LABEL(bpf_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(bpf_hbox), bpf_lbl, FALSE, FALSE, 0);

    g_bpf_switch = gtk_switch_new();
    gtk_widget_set_tooltip_text(g_bpf_switch, "700\xe2\x80\x93" "2300 Hz bandpass filter on TX output");
    gtk_widget_set_valign(g_bpf_switch, GTK_ALIGN_CENTER);
    g_signal_connect(g_bpf_switch, "state-set", G_CALLBACK(on_bpf_switch_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bpf_hbox), g_bpf_switch, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(scontent), bpf_hbox, FALSE, FALSE, 0);

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

    gtk_box_pack_start(GTK_BOX(vbox), btn_hbox, FALSE, FALSE, 0);

    /* ── input meter + spectrum + output meter (side by side) ────────── */
    GtkWidget* meter_spec_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

    GtkWidget* mic_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_mic_slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    gtk_range_set_inverted(GTK_RANGE(g_mic_slider), TRUE);   /* 100 at top */
    gtk_range_set_value(GTK_RANGE(g_mic_slider), 50);
    gtk_scale_set_draw_value(GTK_SCALE(g_mic_slider), FALSE);
    gtk_widget_set_size_request(g_mic_slider, 30, -1);
    gtk_widget_set_tooltip_text(g_mic_slider, "TX mic input level");
    g_signal_connect(g_mic_slider, "value-changed",
                     G_CALLBACK(on_mic_level_changed), NULL);
    gtk_box_pack_start(GTK_BOX(mic_vbox), g_mic_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(mic_vbox), gtk_label_new("Mic"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), mic_vbox, FALSE, FALSE, 0);

    g_meter_in = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_meter_in, FALSE, FALSE, 0);

    GtkWidget* spec_waterfall_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

    g_spectrum = spectrum_widget_new();
    gtk_box_pack_start(GTK_BOX(spec_waterfall_vbox), g_spectrum, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(g_spectrum, TRUE); /* hidden for now; remove this line to restore */

    g_waterfall = waterfall_widget_new();
    gtk_box_pack_start(GTK_BOX(spec_waterfall_vbox), g_waterfall, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), spec_waterfall_vbox, TRUE, TRUE, 0);

    g_meter_out = meter_widget_new();
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), g_meter_out, FALSE, FALSE, 0);

    GtkWidget* tx_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_tx_slider = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, 0, 100, 1);
    gtk_range_set_inverted(GTK_RANGE(g_tx_slider), TRUE);   /* 100 at top */
    gtk_range_set_value(GTK_RANGE(g_tx_slider), 50);
    gtk_scale_set_draw_value(GTK_SCALE(g_tx_slider), FALSE);
    gtk_widget_set_size_request(g_tx_slider, 30, -1);
    gtk_widget_set_tooltip_text(g_tx_slider, "TX output level");
    g_signal_connect(g_tx_slider, "value-changed",
                     G_CALLBACK(on_tx_level_changed), NULL);
    gtk_box_pack_start(GTK_BOX(tx_vbox), g_tx_slider, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(tx_vbox), gtk_label_new("TX"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(meter_spec_hbox), tx_vbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), meter_spec_hbox, TRUE, TRUE, 0);

    /* ── rig status line ────────────────────────────────────────────── */
    g_rig_status_lbl = gtk_label_new("Rig: not connected");
    gtk_label_set_xalign(GTK_LABEL(g_rig_status_lbl), 0.5);
    gtk_box_pack_start(GTK_BOX(vbox), g_rig_status_lbl, FALSE, FALSE, 0);

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

    /* ── auto-connect to rig if settings were saved ─────────────── */
    rig_auto_connect(GTK_WINDOW(window));

    /* ── connect to FreeDV Reporter ─────────────────────────────── */
    reporter_restart();
}
