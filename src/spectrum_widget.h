#pragma once
#include <gtk/gtk.h>

/* Create a spectrum-display GtkDrawingArea.
 *   - Call spectrum_widget_update() regularly to push fresh magnitude data.
 *   - mag_dB[] contains n_bins values in dB (0 dB = full-scale).
 *   - sample_rate is used for the frequency-axis labels.
 *   - Passing nullptr or n_bins==0 clears the display.
 */
GtkWidget* spectrum_widget_new(void);
void       spectrum_widget_update(GtkWidget* widget, const float* mag_dB, int n_bins,
                                  float sample_rate);
