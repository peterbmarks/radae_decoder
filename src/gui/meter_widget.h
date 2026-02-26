#pragma once
#include <gtk/gtk.h>

/* Create a mono bar-meter GtkDrawingArea.
 *   - Call meter_widget_update() regularly (e.g. from a g_timeout_add callback)
 *     to push a fresh RMS level in.
 *   - Peak-hold and peak-fall logic is handled internally.
 */
GtkWidget* meter_widget_new(void);
void       meter_widget_update(GtkWidget* widget, float level);
