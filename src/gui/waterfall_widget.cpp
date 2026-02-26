#include "waterfall_widget.h"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <vector>

/* ── internal state ─────────────────────────────────────────────────────── */

static constexpr const char* STATE_KEY = "waterfall-state";
static constexpr float       DB_MIN    = -80.f;
static constexpr float       DB_MAX    =   0.f;
static constexpr int         N_BINS    = 256;
static constexpr int         N_ROWS    = 200;

struct WaterfallState {
    std::vector<uint32_t> pixels;                // N_BINS * N_ROWS, ARGB32
    uint32_t              lut[256];              // dB-to-color lookup table
    cairo_surface_t*      surface  = nullptr;
    float                 sample_rate = 8000.f;
};

/* ── colour lookup table ────────────────────────────────────────────────── */

static void build_lut(uint32_t lut[256])
{
    for (int i = 0; i < 256; i++) {
        float t = static_cast<float>(i) / 255.f;

        float r, g, b;
        if (t < 0.25f) {
            float s = t / 0.25f;
            r = 0.f;
            g = 0.f;
            b = s * 0.5f;                       // black → dark blue
        } else if (t < 0.50f) {
            float s = (t - 0.25f) / 0.25f;
            r = 0.f;
            g = s * 0.8f;
            b = 0.5f + s * 0.5f;                // dark blue → cyan
        } else if (t < 0.75f) {
            float s = (t - 0.50f) / 0.25f;
            r = s;
            g = 0.8f + s * 0.2f;
            b = 1.0f - s;                       // cyan → yellow
        } else {
            float s = (t - 0.75f) / 0.25f;
            r = 1.0f;
            g = 1.0f;
            b = s;                               // yellow → white
        }

        auto R = static_cast<uint8_t>(r * 255.f);
        auto G = static_cast<uint8_t>(g * 255.f);
        auto B = static_cast<uint8_t>(b * 255.f);

        /* CAIRO_FORMAT_ARGB32: native-endian packed 0xAARRGGBB on LE x86 */
        lut[i] = (0xFFu << 24) | (static_cast<uint32_t>(R) << 16)
               | (static_cast<uint32_t>(G) << 8) | B;
    }
}

/* ── draw callback ──────────────────────────────────────────────────────── */

static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer /*data*/)
{
    auto* st = static_cast<WaterfallState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st || !st->surface) return FALSE;

    GtkAllocation alloc;
    gtk_widget_get_allocation(widget, &alloc);
    double W = alloc.width;
    double H = alloc.height;

    /* margins match spectrum_widget for horizontal alignment */
    constexpr double ml = 36;   // left  (same as spectrum dB-label area)
    constexpr double mr = 10;   // right
    constexpr double mt =  2;   // top   (tight — spectrum is directly above)
    constexpr double mb =  2;   // bottom

    double pw = W - ml - mr;
    double ph = H - mt - mb;
    if (pw < 10 || ph < 10) return FALSE;

    /* overall background */
    cairo_set_source_rgb(cr, 0.11, 0.11, 0.14);
    cairo_paint(cr);

    /* scale the N_BINS x N_ROWS pixel buffer to fill the plot area */
    cairo_save(cr);
    cairo_translate(cr, ml, mt);
    cairo_scale(cr, pw / N_BINS, ph / N_ROWS);
    cairo_set_source_surface(cr, st->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);

    /* plot border */
    cairo_set_source_rgb(cr, 0.30, 0.30, 0.35);
    cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, ml + 0.5, mt + 0.5, pw - 1, ph - 1);
    cairo_stroke(cr);

    return FALSE;
}

/* ── public API ─────────────────────────────────────────────────────────── */

GtkWidget* waterfall_widget_new(void)
{
    GtkWidget* da = gtk_drawing_area_new();

    auto* state = new WaterfallState{};
    build_lut(state->lut);

    /* allocate pixel buffer and fill with background colour (LUT index 0) */
    state->pixels.resize(static_cast<size_t>(N_BINS) * N_ROWS, state->lut[0]);

    /* create a Cairo image surface that wraps the pixel vector's memory */
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, N_BINS);
    state->surface = cairo_image_surface_create_for_data(
        reinterpret_cast<unsigned char*>(state->pixels.data()),
        CAIRO_FORMAT_ARGB32,
        N_BINS, N_ROWS, stride);

    g_object_set_data_full(G_OBJECT(da), STATE_KEY, state,
        [](gpointer p) {
            auto* st = static_cast<WaterfallState*>(p);
            if (st->surface) cairo_surface_destroy(st->surface);
            delete st;
        });

    g_signal_connect(da, "draw", G_CALLBACK(on_draw), nullptr);
    gtk_widget_set_size_request(da, 240, 100);

    return da;
}

void waterfall_widget_update(GtkWidget* widget, const float* mag_dB, int n_bins,
                              float sample_rate)
{
    auto* st = static_cast<WaterfallState*>(
        g_object_get_data(G_OBJECT(widget), STATE_KEY));
    if (!st) return;

    st->sample_rate = sample_rate;

    /* nullptr / 0 bins → clear the display */
    if (!mag_dB || n_bins <= 0) {
        std::fill(st->pixels.begin(), st->pixels.end(), st->lut[0]);
        if (st->surface) cairo_surface_mark_dirty(st->surface);
        gtk_widget_queue_draw(widget);
        return;
    }

    /* scroll: shift all rows down by one */
    std::memmove(st->pixels.data() + N_BINS,
                 st->pixels.data(),
                 static_cast<size_t>((N_ROWS - 1) * N_BINS) * sizeof(uint32_t));

    /* write the new top row from magnitude data */
    int count = std::min(n_bins, N_BINS);
    for (int i = 0; i < count; i++) {
        float clamped = std::max(DB_MIN, std::min(DB_MAX, mag_dB[i]));
        float t = (clamped - DB_MIN) / (DB_MAX - DB_MIN);
        int idx = static_cast<int>(t * 255.f);
        idx = std::max(0, std::min(255, idx));
        st->pixels[static_cast<size_t>(i)] = st->lut[idx];
    }
    for (int i = count; i < N_BINS; i++)
        st->pixels[static_cast<size_t>(i)] = st->lut[0];

    if (st->surface) cairo_surface_mark_dirty(st->surface);
    gtk_widget_queue_draw(widget);
}
