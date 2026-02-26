#include "meter_widget.h"
#include <cmath>
#include <algorithm>
#include <cstdio>

/* ── internal state ─────────────────────────────────────────────────────── */

struct MeterState {
    float level = 0.f;   // current RMS  (linear 0..1)
    float peak  = 0.f;   // peak-hold    (linear 0..1)
    int   timer = 0;     // hold-frame counter
};

static constexpr const char* STATE_KEY       = "meter-state";
static constexpr int         PEAK_HOLD      = 45;     // frames before fall (~1.5 s @ 30 fps)
static constexpr float       PEAK_DECAY     = 0.925f; // per-frame multiplier during fall

/* ── dB / position helpers ──────────────────────────────────────────────── */

static constexpr float DB_MIN = -60.f;
static constexpr float DB_MAX =   0.f;

/* linear amplitude  →  0..1 meter position (bottom = 0, top = 1) */
static float level_to_pos(float level)
{
    if (level < 1e-6f) return 0.f;
    float db = 20.f * std::log10(level);
    db = std::max(DB_MIN, std::min(DB_MAX, db));
    return (db - DB_MIN) / (DB_MAX - DB_MIN);
}

/* ── cairo helpers ──────────────────────────────────────────────────────── */

static constexpr int TICKS[] = { 0, -12, -24, -36, -48, -60 };
static constexpr int NUM_TICKS = 6;

static float db_to_pos(int db)
{
    return (static_cast<float>(db) - DB_MIN) / (DB_MAX - DB_MIN);
}

/* ── draw one vertical bar ──────────────────────────────────────────────── */

static void draw_bar(cairo_t* cr,
                     double x, double y, double w, double h,
                     float  fill, float  peak)
{
    /* background */
    cairo_set_source_rgb(cr, 0.17, 0.17, 0.20);
    cairo_rectangle(cr, x, y, w, h);
    cairo_fill(cr);

    /* subtle border */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);

    /* tick grid lines */
    cairo_set_source_rgba(cr, 0.40, 0.40, 0.44, 0.25);
    cairo_set_line_width(cr, 0.6);
    for (int i = 0; i < NUM_TICKS; ++i) {
        double ty = y + h - db_to_pos(TICKS[i]) * h;
        cairo_move_to(cr, x + 1, ty);
        cairo_line_to(cr, x + w - 1, ty);
        cairo_stroke(cr);
    }

    /* filled portion with vertical gradient */
    double fill_h = fill * h;
    if (fill_h > 0.5) {
        cairo_pattern_t* grad =
            cairo_pattern_create_linear(0, y + h, 0, y);
        cairo_pattern_add_color_stop_rgb(grad, 0.00, 0.00, 0.65, 0.18);
        cairo_pattern_add_color_stop_rgb(grad, 0.50, 0.05, 0.88, 0.10);
        cairo_pattern_add_color_stop_rgb(grad, 0.68, 0.70, 0.92, 0.05);
        cairo_pattern_add_color_stop_rgb(grad, 0.80, 0.95, 0.80, 0.02);
        cairo_pattern_add_color_stop_rgb(grad, 0.90, 1.00, 0.45, 0.02);
        cairo_pattern_add_color_stop_rgb(grad, 1.00, 1.00, 0.08, 0.05);

        cairo_set_source(cr, grad);
        cairo_rectangle(cr, x, y + h - fill_h, w, fill_h);
        cairo_fill(cr);
        cairo_pattern_destroy(grad);
    }

    /* peak-hold line */
    if (peak > 0.004f) {
        double py = y + h - peak * h;
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.88);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, x + 1, py);
        cairo_line_to(cr, x + w - 1, py);
        cairo_stroke(cr);
    }
}

/* ── draw dB labels to the right of the bar ────────────────────────────── */

static void draw_db_labels(cairo_t* cr, double x, double y, double w, double h)
{
    cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
    cairo_set_font_size(cr, 8.0);

    for (int i = 0; i < NUM_TICKS; ++i) {
        double ty = y + h - db_to_pos(TICKS[i]) * h;

        char label[8];
        snprintf(label, sizeof label, "%d", TICKS[i]);

        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        cairo_move_to(cr,
                      x + (w - ext.width) * 0.5 - ext.x_bearing,
                      ty - ext.height * 0.5 - ext.y_bearing);
        cairo_show_text(cr, label);
    }
}

/* ── main draw callback ─────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer /*data*/)
{
    auto* st = static_cast<MeterState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double W = alloc.width;
    double H = alloc.height;

    /* ── layout ──────────────────────────────────────────────────── */
    constexpr double mt = 6;    // top margin
    constexpr double mb = 4;    // bottom margin
    constexpr double ml = 2;    // left margin
    constexpr double label_w = 22; // dB label column
    constexpr double gap = 2;

    double bar_h = H - mt - mb;
    double bar_w = W - ml - gap - label_w - 2;
    if (bar_w < 8) bar_w = 8;

    /* ── overall background ──────────────────────────────────────── */
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.14);
    cairo_paint(cr);

    /* ── bar ─────────────────────────────────────────────────────── */
    draw_bar(cr, ml, mt, bar_w, bar_h,
             level_to_pos(st->level), level_to_pos(st->peak));

    /* ── dB labels ───────────────────────────────────────────────── */
    draw_db_labels(cr, ml + bar_w + gap, mt, label_w, bar_h);

    return FALSE;
}

/* ── public API ─────────────────────────────────────────────────────────── */

GtkWidget* meter_widget_new(void)
{
    GtkWidget* da = gtk_drawing_area_new();

    auto* state = new MeterState{};
    g_object_set_data_full(G_OBJECT(da), STATE_KEY, state,
        [](gpointer p){ delete static_cast<MeterState*>(p); });

    g_signal_connect(da, "draw", G_CALLBACK(on_draw), nullptr);

    gtk_widget_set_size_request(da, 40, 120);
    return da;
}

void meter_widget_update(GtkWidget* widget, float level)
{
    auto* st = static_cast<MeterState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return;

    st->level = level;

    /* peak-hold / fall logic */
    if (level >= st->peak) {
        st->peak  = level;
        st->timer = PEAK_HOLD;
    } else {
        if (st->timer > 0)  --st->timer;
        else                 st->peak *= PEAK_DECAY;
        if (st->peak < 1e-7f) st->peak = 0.f;
    }

    gtk_widget_queue_draw(widget);
}
