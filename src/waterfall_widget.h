#pragma once
#include <gtk/gtk.h>

/* Create a waterfall-display GtkDrawingArea.
 *   - Call waterfall_widget_update() regularly to push fresh magnitude data.
 *   - mag_dB[] contains n_bins values in dB (0 dB = full-scale).
 *   - sample_rate is used for frequency-axis scaling.
 *   - Passing nullptr or n_bins==0 clears the display.
 */
GtkWidget* waterfall_widget_new(void);
void       waterfall_widget_update(GtkWidget* widget, const float* mag_dB, int n_bins,
                                    float sample_rate);
