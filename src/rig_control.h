#pragma once

#include <gtk/gtk.h>
#include <string>

/* Load all hamlib backends and enumerate serial ports.
   Call once, early in program start (before create_dialog). */
void rig_control_init();

/* Build and return the (initially hidden) Rig Control dialog.
   parent_window must already exist. */
GtkWidget* rig_control_create_dialog(GtkWidget* parent_window);

/* ── config helpers (called by main's save/restore) ──────────────────── */

/* Returns the currently selected rig model_id as a decimal string, or "". */
std::string rig_config_get_model_id();

/* Returns the currently selected (or typed) serial port path, or "". */
std::string rig_config_get_port();

/* Returns the currently selected baud rate string, or "". */
std::string rig_config_get_baud();

/* Restores previously saved selections into the dialog widgets. */
void rig_config_restore(const std::string& model_id,
                        const std::string& port,
                        const std::string& baud);

/* Register a callback that is called whenever the user changes a rig
   setting (model, port, or baud).  Pass main's save_config here so that
   rig settings are written to disk immediately on change. */
void rig_control_set_save_callback(void (*cb)());

/* If a radio model and serial port are already configured, attempt to
   connect automatically.  On failure an error alert is shown parented to
   `parent`.  Call this once after rig_config_restore(). */
void rig_auto_connect(GtkWindow* parent);
