#include "spectrum_widget.h"
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <cstring>

/* ── internal state ─────────────────────────────────────────────────────── */

struct SpectrumState {
    std::vector<float> bins;         // magnitude in dB, one per bin
    float              sample_rate = 8000.f;
};

static constexpr const char* STATE_KEY  = "spectrum-state";
static constexpr float       DB_MIN     = -80.f;
static constexpr float       DB_MAX     =   0.f;

/* ── draw callback ──────────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer /*data*/)
{
    auto* st = static_cast<SpectrumState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double W = alloc.width;
    double H = alloc.height;

    /* ── layout margins ──────────────────────────────────────────── */
    constexpr double ml = 36;   // left (dB labels)
    constexpr double mr = 10;   // right
    constexpr double mt =  6;   // top
    constexpr double mb = 20;   // bottom (freq labels)

    double pw = W - ml - mr;    // plot width
    double ph = H - mt - mb;    // plot height
    if (pw < 10 || ph < 10) return FALSE;

    /* ── overall background ──────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.14);
    cairo_paint(cr);

    /* ── plot area background ────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.14, 0.14, 0.17);
    cairo_rectangle(cr, ml, mt, pw, ph);
    cairo_fill(cr);

    /* ── dB grid lines ───────────────────────────────────────────── */
    cairo_set_line_width(cr, 0.6);
    cairo_set_font_size(cr, 9.0);

    constexpr int db_ticks[] = { 0, -20, -40, -60, -80 };
    for (int db : db_ticks) {
        double frac = (static_cast<double>(db) - DB_MIN) / (DB_MAX - DB_MIN);
        double y = mt + ph - frac * ph;

        /* grid line */
        cairo_set_source_rgba(cr, 0.35, 0.35, 0.40, 0.4);
        cairo_move_to(cr, ml, y);
        cairo_line_to(cr, ml + pw, y);
        cairo_stroke(cr);

        /* label */
        char label[8];
        std::snprintf(label, sizeof label, "%d", db);
        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 0.50, 0.50, 0.55);
        cairo_text_extents(cr, label, &ext);
        cairo_move_to(cr, ml - ext.width - 4, y + ext.height * 0.35);
        cairo_show_text(cr, label);
    }

    /* ── frequency grid lines ────────────────────────────────────── */
    float nyquist = st->sample_rate * 0.5f;
    constexpr float freq_ticks[] = { 0, 1000, 2000, 3000, 4000 };
    for (float fhz : freq_ticks) {
        if (fhz > nyquist) break;
        double xfrac = static_cast<double>(fhz) / nyquist;
        double x = ml + xfrac * pw;

        /* grid line */
        cairo_set_source_rgba(cr, 0.35, 0.35, 0.40, 0.4);
        cairo_move_to(cr, x, mt);
        cairo_line_to(cr, x, mt + ph);
        cairo_stroke(cr);

        /* label */
        char label[8];
        if (fhz >= 1000)
            std::snprintf(label, sizeof label, "%dk", static_cast<int>(fhz / 1000));
        else
            std::snprintf(label, sizeof label, "%d", static_cast<int>(fhz));

        cairo_text_extents_t ext;
        cairo_set_source_rgb(cr, 0.50, 0.50, 0.55);
        cairo_text_extents(cr, label, &ext);
        cairo_move_to(cr, x - ext.width * 0.5, mt + ph + 14);
        cairo_show_text(cr, label);
    }

    /* ── spectrum trace ──────────────────────────────────────────── */
    int nb = static_cast<int>(st->bins.size());
    if (nb < 2) return FALSE;

    /* build the polygon path along the top of the spectrum */
    auto bin_x = [&](int i) -> double {
        return ml + (static_cast<double>(i) / (nb - 1)) * pw;
    };
    auto db_y = [&](float db) -> double {
        float clamped = std::max(DB_MIN, std::min(DB_MAX, db));
        double frac = (static_cast<double>(clamped) - DB_MIN) / (DB_MAX - DB_MIN);
        return mt + ph - frac * ph;
    };

    /* filled area: start at bottom-left, trace spectrum, close at bottom-right */
    cairo_move_to(cr, ml, mt + ph);
    for (int i = 0; i < nb; i++)
        cairo_line_to(cr, bin_x(i), db_y(st->bins[static_cast<size_t>(i)]));
    cairo_line_to(cr, ml + pw, mt + ph);
    cairo_close_path(cr);

    /* gradient fill */
    cairo_pattern_t* grad = cairo_pattern_create_linear(0, mt + ph, 0, mt);
    cairo_pattern_add_color_stop_rgba(grad, 0.00, 0.00, 0.55, 0.30, 0.25);
    cairo_pattern_add_color_stop_rgba(grad, 0.40, 0.00, 0.70, 0.50, 0.45);
    cairo_pattern_add_color_stop_rgba(grad, 0.80, 0.10, 0.85, 0.70, 0.60);
    cairo_pattern_add_color_stop_rgba(grad, 1.00, 0.30, 1.00, 0.90, 0.75);
    cairo_set_source(cr, grad);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(grad);

    /* stroke the top edge of the spectrum */
    cairo_set_source_rgba(cr, 0.20, 0.90, 0.70, 0.9);
    cairo_set_line_width(cr, 1.2);
    /* re-trace just the spectrum line (not the bottom) for the stroke */
    cairo_new_path(cr);
    cairo_move_to(cr, bin_x(0), db_y(st->bins[0]));
    for (int i = 1; i < nb; i++)
        cairo_line_to(cr, bin_x(i), db_y(st->bins[static_cast<size_t>(i)]));
    cairo_stroke(cr);

    /* ── plot border ─────────────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, ml + 0.5, mt + 0.5, pw - 1, ph - 1);
    cairo_stroke(cr);

    return FALSE;
}

/* ── public API ─────────────────────────────────────────────────────────── */

GtkWidget* spectrum_widget_new(void)
{
    GtkWidget* da = gtk_drawing_area_new();

    auto* state = new SpectrumState{};
    g_object_set_data_full(G_OBJECT(da), STATE_KEY, state,
        [](gpointer p){ delete static_cast<SpectrumState*>(p); });

    g_signal_connect(da, "draw", G_CALLBACK(on_draw), nullptr);

    gtk_widget_set_size_request(da, 240, 150);
    return da;
}

void spectrum_widget_update(GtkWidget* widget, const float* mag_dB, int n_bins,
                            float sample_rate)
{
    auto* st = static_cast<SpectrumState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return;

    if (mag_dB && n_bins > 0) {
        st->bins.assign(mag_dB, mag_dB + n_bins);
        st->sample_rate = sample_rate;
    } else {
        st->bins.clear();
    }

    gtk_widget_queue_draw(widget);
}
