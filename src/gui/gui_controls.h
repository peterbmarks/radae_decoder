#pragma once

#include <gtk/gtk.h>
#include <string>
#include <cstdint>

/* Status / button helpers */
void set_status(const char* msg);
void set_btn_state(bool capturing);

/* TX level slider callbacks */
void on_mic_level_changed(GtkRange* range, gpointer data);
void on_tx_level_changed(GtkRange* range, gpointer data);

/* Rig helpers */
void     update_rig_status_label();
uint64_t rig_freq_hz();

/* Reporter */
void reporter_restart();
void refresh_reporter_list();

/* Decoder / encoder / passthrough lifecycle */
void stop_all();
void start_decoder(int in_idx, int out_idx);
void start_encoder(int mic_idx, int radio_idx);
void start_decoder_file(const std::string& wav_path, int out_idx);
void start_passthrough(int in_idx, int out_idx);

/* Settings dialog: standalone mic input level meter.
   start_mic_monitor() no-ops unless the Settings dialog is visible and no
   encoder is already using the mic device; safe to call any time. */
void start_mic_monitor();
void stop_mic_monitor();

/* Settings dialog: momentary 1kHz test tone to the selected speaker output.
   Call start on button-press, stop on button-release. */
void start_output_test_tone();
void stop_output_test_tone();
