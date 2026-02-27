#pragma once

#include <gtk/gtk.h>

/* Timer callback — declared here because start_decoder/start_encoder pass it
   to g_timeout_add before gui_callbacks.cpp is in scope. */
gboolean on_meter_tick(gpointer data);

/* Device selection */
void on_input_combo_changed(GtkComboBox* combo, gpointer data);
void on_output_combo_changed(GtkComboBox* combo, gpointer data);
void on_tx_combo_changed(GtkComboBox* combo, gpointer data);

/* Station settings */
void on_callsign_changed(GtkEditable* e, gpointer data);
void on_gridsquare_changed(GtkEditable* e, gpointer data);

/* Mode toggles */
gboolean on_tx_switch_changed(GtkSwitch* sw, gboolean state, gpointer data);
gboolean on_bpf_switch_changed(GtkSwitch* sw, gboolean state, gpointer data);

/* Buttons */
void on_record_clicked(GtkButton* btn, gpointer data);
void on_start_stop(GtkButton* btn, gpointer data);
void on_refresh(GtkButton* btn, gpointer data);

/* Window lifecycle */
void on_window_destroy(GtkWidget* w, gpointer data);

/* Menu items */
void on_open_file(GtkMenuItem* item, gpointer parent_window);
void on_quit(GtkMenuItem* item, gpointer app);
void on_settings(GtkMenuItem* item, gpointer data);
void on_rig_control(GtkMenuItem* item, gpointer data);
void on_reporter(GtkMenuItem* item, gpointer data);

/* Reporter message */
void on_send_message(GtkButton* btn, gpointer data);
