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

/* Returns either:
   - selected serial port path, or
   - "tcp://host:port" when TCP CAT is selected,
   or "". */
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

/* If a radio model and CAT endpoint (serial or TCP) are already configured,
   attempt to connect automatically.  On failure an error alert is shown
   parented to `parent`.  Call this once after rig_config_restore(). */
void rig_auto_connect(GtkWindow* parent);

/* ── real-time state queries (for the main-window status line) ────────── */
bool        rig_is_connected();
bool        rig_is_ptt_supported();
std::string rig_get_current_freq();   /* e.g. "14.225.000 MHz", or "" */
std::string rig_get_current_mode();   /* e.g. "USB", or ""            */
bool        rig_get_ptt_on();

/* Key or un-key the rig PTT.  No-op when no rig is connected. */
void rig_control_set_ptt(bool on);

/* Release PTT, wait for the serial port to flush, then close the rig.
   Call once at application exit in place of rig_control_set_ptt(false). */
void rig_control_cleanup();
