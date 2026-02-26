#include "rig_control.h"

#include <gtk/gtk.h>
#include <hamlib/rig.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glob.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/* ── rig enumeration data ────────────────────────────────────────────── */

struct RigEntry {
    rig_model_t model_id;
    std::string label;   /* "Manufacturer  Model" */
};

static std::vector<RigEntry>    g_rig_list;
static std::vector<std::string> g_serial_ports;

static int rig_enum_cb(const struct rig_caps* caps, rig_ptr_t /*data*/)
{
    if (!caps || !caps->mfg_name || !caps->model_name) return 1;
    RigEntry e;
    e.model_id = caps->rig_model;
    e.label    = std::string(caps->mfg_name) + "  " + caps->model_name;
    g_rig_list.push_back(std::move(e));
    return 1;
}

void rig_control_init()
{
    rig_set_debug(RIG_DEBUG_NONE);
    rig_load_all_backends();
    rig_list_foreach(rig_enum_cb, nullptr);

    std::sort(g_rig_list.begin(), g_rig_list.end(),
              [](const RigEntry& a, const RigEntry& b){ return a.label < b.label; });

    /* Enumerate likely serial port nodes */
    auto glob_add = [](const char* pattern) {
        glob_t gl{};
        if (glob(pattern, 0, nullptr, &gl) == 0) {
            for (size_t i = 0; i < gl.gl_pathc; ++i)
                g_serial_ports.push_back(gl.gl_pathv[i]);
        }
        globfree(&gl);
    };

    glob_add("/dev/ttyUSB*");
    glob_add("/dev/ttyACM*");
    glob_add("/dev/rfcomm*");
    glob_add("/dev/ttyS[0-9]");
    glob_add("/dev/ttyS[1-9][0-9]");
}

/* ── widget globals ──────────────────────────────────────────────────── */

static GtkWidget* g_rig_combo      = nullptr;
static GtkWidget* g_port_combo     = nullptr;
static GtkWidget* g_baud_combo     = nullptr;
static GtkWidget* g_connect_btn    = nullptr;
static GtkWidget* g_status_lbl     = nullptr;
static GtkWidget* g_freq_entry     = nullptr;
static GtkWidget* g_mode_entry     = nullptr;
static GtkWidget* g_test_tx_btn    = nullptr;

static RIG*  g_rig       = nullptr;
static bool  g_connected = false;
static bool  g_ptt_on    = false;

/* ── background poll thread ──────────────────────────────────────────── */
static std::thread            g_poll_thread;
static std::mutex             g_rig_mutex;    /* guards g_rig and all hamlib calls */
static std::mutex             g_cache_mutex;  /* guards g_cached_freq/mode */
static std::atomic<bool>      g_poll_running{false};

static std::string g_cached_freq;   /* last polled frequency string, e.g. "14.225.000 MHz" */
static std::string g_cached_mode;   /* last polled mode string, e.g. "USB" */

static void (*g_save_cb)() = nullptr;

void rig_control_set_save_callback(void (*cb)()) { g_save_cb = cb; }

static void fire_save() { if (g_save_cb) g_save_cb(); }

/* ── helpers ─────────────────────────────────────────────────────────── */

static void set_status(const char* msg)
{
    if (g_status_lbl)
        gtk_label_set_text(GTK_LABEL(g_status_lbl), msg);
}

static void apply_connected_state(bool connected)
{
    g_connected = connected;
    gtk_button_set_label(GTK_BUTTON(g_connect_btn),
                         connected ? "Disconnect" : "Connect");
    gtk_widget_set_sensitive(g_test_tx_btn, connected);
    gtk_widget_set_sensitive(g_rig_combo,   !connected);
    gtk_widget_set_sensitive(g_port_combo,  !connected);
    gtk_widget_set_sensitive(g_baud_combo,  !connected);
    if (!connected) {
        g_cached_freq.clear();
        g_cached_mode.clear();
        g_ptt_on = false;
        gtk_entry_set_text(GTK_ENTRY(g_freq_entry), "");
        gtk_entry_set_text(GTK_ENTRY(g_mode_entry), "");
    }
}

/* ── background poll thread ──────────────────────────────────────────── */

/* Called on the GTK main thread (via g_idle_add) to push cached values
   into the dialog's read-only entry widgets. */
static gboolean update_rig_entries(gpointer /*data*/)
{
    std::string freq, mode;
    {
        std::lock_guard<std::mutex> lk(g_cache_mutex);
        freq = g_cached_freq;
        mode = g_cached_mode;
    }
    if (g_freq_entry)
        gtk_entry_set_text(GTK_ENTRY(g_freq_entry), freq.c_str());
    if (g_mode_entry)
        gtk_entry_set_text(GTK_ENTRY(g_mode_entry), mode.c_str());
    return FALSE; /* one-shot */
}

static void rig_poll_thread_func()
{
    while (g_poll_running.load()) {
        /* ── query the radio (blocking serial I/O, off the GTK thread) ── */
        std::string new_freq, new_mode;
        {
            std::lock_guard<std::mutex> lk(g_rig_mutex);
            if (!g_rig || !g_connected || !g_poll_running.load())
                break;

            freq_t freq = 0;
            if (rig_get_freq(g_rig, RIG_VFO_CURR, &freq) == RIG_OK) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.6f MHz", freq / 1e6);
                new_freq = buf;
            }

            rmode_t   mode  = RIG_MODE_NONE;
            pbwidth_t width = 0;
            if (rig_get_mode(g_rig, RIG_VFO_CURR, &mode, &width) == RIG_OK)
                new_mode = rig_strrmode(mode);
        }

        /* ── update cache ──────────────────────────────────────────────── */
        {
            std::lock_guard<std::mutex> lk(g_cache_mutex);
            g_cached_freq = new_freq;
            g_cached_mode = new_mode;
        }

        /* Push widget update to the GTK main thread */
        g_idle_add(update_rig_entries, nullptr);

        /* ── sleep 5 s in 100 ms slices so we can exit promptly ────────── */
        for (int i = 0; i < 50 && g_poll_running.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

/* ── connect / disconnect ────────────────────────────────────────────── */

static void do_disconnect()
{
    /* Stop the background poll thread before touching the rig handle. */
    g_poll_running.store(false);
    if (g_poll_thread.joinable())
        g_poll_thread.join();

    {
        std::lock_guard<std::mutex> lk(g_rig_mutex);
        if (g_rig) {
            rig_close(g_rig);
            rig_cleanup(g_rig);
            g_rig = nullptr;
        }
    }
    apply_connected_state(false);
    set_status("Disconnected.");
}

/* Attempt to open the rig using the current widget selections.
   Returns an empty string on success, or a human-readable error message. */
static std::string do_connect()
{
    int model_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_rig_combo));
    if (model_idx < 0 || model_idx >= static_cast<int>(g_rig_list.size()))
        return "Select a radio model first.";

    gchar* port_str = gtk_combo_box_text_get_active_text(
                          GTK_COMBO_BOX_TEXT(g_port_combo));
    if (!port_str || port_str[0] == '\0') {
        g_free(port_str);
        return "Select a serial port first.";
    }

    gchar* baud_str = gtk_combo_box_text_get_active_text(
                          GTK_COMBO_BOX_TEXT(g_baud_combo));
    int baud = baud_str ? std::atoi(baud_str) : 9600;
    g_free(baud_str);

    rig_model_t model_id = g_rig_list[static_cast<size_t>(model_idx)].model_id;

    g_rig = rig_init(model_id);
    if (!g_rig) {
        g_free(port_str);
        return "rig_init() failed \xe2\x80\x94 invalid model?";
    }

    std::strncpy(g_rig->state.rigport.pathname, port_str,
                 HAMLIB_FILPATHLEN - 1);
    g_rig->state.rigport.pathname[HAMLIB_FILPATHLEN - 1] = '\0';
    g_rig->state.rigport.parm.serial.rate = baud;
    g_free(port_str);

    int ret = rig_open(g_rig);
    if (ret != RIG_OK) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "rig_open: %s", rigerror(ret));
        rig_cleanup(g_rig);
        g_rig = nullptr;
        return buf;
    }

    apply_connected_state(true);
    set_status("Connected.");
    g_poll_running.store(true);
    g_poll_thread = std::thread(rig_poll_thread_func);
    return "";
}

static void on_connect_clicked(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (g_connected) { do_disconnect(); return; }

    std::string err = do_connect();
    if (!err.empty())
        set_status(("Error: " + err).c_str());
}

/* ── PTT test ────────────────────────────────────────────────────────── */

static gboolean on_ptt_off(gpointer /*data*/)
{
    if (g_rig && g_connected)
        rig_set_ptt(g_rig, RIG_VFO_CURR, RIG_PTT_OFF);
    g_ptt_on = false;
    gtk_widget_set_sensitive(g_test_tx_btn, TRUE);
    gtk_button_set_label(GTK_BUTTON(g_test_tx_btn), "Test TX (1 s)");
    set_status("Connected.  PTT test complete.");
    return FALSE; /* one-shot */
}

static void on_test_tx(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (!g_rig || !g_connected) { set_status("Not connected."); return; }

    int ret = rig_set_ptt(g_rig, RIG_VFO_CURR, RIG_PTT_ON);
    if (ret != RIG_OK) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "Error: PTT on: %s", rigerror(ret));
        set_status(buf);
        return;
    }
    g_ptt_on = true;
    gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
    gtk_button_set_label(GTK_BUTTON(g_test_tx_btn), "TX\xe2\x80\xa6");  /* TX… */
    set_status("PTT ON \xe2\x80\x94 will turn off in 1 s\xe2\x80\xa6");
    g_timeout_add(1000, on_ptt_off, nullptr);
}

/* ── auto-connect at launch ──────────────────────────────────────────── */

void rig_auto_connect(GtkWindow* parent)
{
    /* Only attempt if both a model and a port have been restored. */
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(g_rig_combo)) < 0)
        return;

    gchar* port_txt = gtk_combo_box_text_get_active_text(
                          GTK_COMBO_BOX_TEXT(g_port_combo));
    bool has_port = port_txt && port_txt[0] != '\0';
    g_free(port_txt);
    if (!has_port) return;

    std::string err = do_connect();
    if (!err.empty()) {
        set_status(("Error: " + err).c_str());

        GtkWidget* dlg = gtk_message_dialog_new(
            parent,
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_ERROR,
            GTK_BUTTONS_CLOSE,
            "Rig Control: auto-connect failed");
        gtk_message_dialog_format_secondary_text(
            GTK_MESSAGE_DIALOG(dlg), "%s", err.c_str());
        gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
    }
}

/* ── dialog construction ─────────────────────────────────────────────── */

GtkWidget* rig_control_create_dialog(GtkWidget* parent_window)
{
    GtkWidget* dlg = gtk_dialog_new_with_buttons(
        "Rig Control",
        GTK_WINDOW(parent_window),
        GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Close", GTK_RESPONSE_CLOSE,
        nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);
    g_signal_connect(dlg, "delete-event",
                     G_CALLBACK(gtk_widget_hide_on_delete), nullptr);
    g_signal_connect_swapped(dlg, "response",
                             G_CALLBACK(gtk_widget_hide), dlg);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    /* ── hamlib version line ──────────────────────────────────────── */
    {
        char buf[160];
        std::snprintf(buf, sizeof buf,
                      "Hamlib version: <b>%s</b>", hamlib_version2);
        GtkWidget* lbl = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(lbl), buf);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* helper: build a label + widget row and pack it */
    auto add_row = [&](const char* label_text, GtkWidget* widget) {
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* lbl  = gtk_label_new(label_text);
        gtk_widget_set_size_request(lbl, 90, -1);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(hbox), lbl,    FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE,  TRUE,  0);
        gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);
    };

    /* ── radio model combo ───────────────────────────────────────── */
    g_rig_combo = gtk_combo_box_text_new();
    for (const auto& e : g_rig_list)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_rig_combo),
                                       e.label.c_str());
    gtk_widget_set_tooltip_text(g_rig_combo, "Select your radio transceiver model");
    g_signal_connect_swapped(g_rig_combo, "changed",
                             G_CALLBACK(fire_save), nullptr);
    add_row("Radio:", g_rig_combo);

    /* ── serial port combo (with editable entry for custom paths) ── */
    g_port_combo = gtk_combo_box_text_new_with_entry();
    for (const auto& p : g_serial_ports)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_port_combo),
                                       p.c_str());
    gtk_widget_set_tooltip_text(g_port_combo,
        "Serial port connected to the rig (you can also type a path)");
    g_signal_connect_swapped(g_port_combo, "changed",
                             G_CALLBACK(fire_save), nullptr);
    add_row("Port:", g_port_combo);

    /* ── baud rate combo ─────────────────────────────────────────── */
    static const char* const k_bauds[] = {
        "300", "1200", "2400", "4800", "9600",
        "19200", "38400", "57600", "115200", nullptr
    };
    g_baud_combo = gtk_combo_box_text_new();
    for (int i = 0; k_bauds[i]; ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_baud_combo),
                                       k_bauds[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_baud_combo), 4); /* 9600 */
    gtk_widget_set_tooltip_text(g_baud_combo, "Serial baud rate");
    g_signal_connect_swapped(g_baud_combo, "changed",
                             G_CALLBACK(fire_save), nullptr);
    add_row("Baud rate:", g_baud_combo);

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* ── connect button + status label ──────────────────────────── */
    {
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

        g_connect_btn = gtk_button_new_with_label("Connect");
        gtk_widget_set_size_request(g_connect_btn, 110, -1);
        g_signal_connect(g_connect_btn, "clicked",
                         G_CALLBACK(on_connect_clicked), nullptr);
        gtk_box_pack_start(GTK_BOX(hbox), g_connect_btn, FALSE, FALSE, 0);

        g_status_lbl = gtk_label_new("Not connected.");
        gtk_label_set_xalign(GTK_LABEL(g_status_lbl), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(g_status_lbl), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(g_status_lbl), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(hbox), g_status_lbl, TRUE, TRUE, 0);

        gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* ── frequency display (read-only entry) ─────────────────────── */
    g_freq_entry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(g_freq_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_freq_entry), "\xe2\x80\x94");
    gtk_widget_set_tooltip_text(g_freq_entry, "Current VFO frequency (polled every second)");
    add_row("Frequency:", g_freq_entry);

    /* ── mode display (read-only entry) ─────────────────────────── */
    g_mode_entry = gtk_entry_new();
    gtk_editable_set_editable(GTK_EDITABLE(g_mode_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(g_mode_entry), "\xe2\x80\x94");
    gtk_widget_set_tooltip_text(g_mode_entry, "Current operating mode (polled every second)");
    add_row("Mode:", g_mode_entry);

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* ── test TX button ──────────────────────────────────────────── */
    g_test_tx_btn = gtk_button_new_with_label("Test TX (1 s)");
    gtk_widget_set_tooltip_text(g_test_tx_btn,
        "Keys the radio for 1 second then releases PTT — use with caution!");
    gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
    g_signal_connect(g_test_tx_btn, "clicked",
                     G_CALLBACK(on_test_tx), nullptr);
    gtk_box_pack_start(GTK_BOX(content), g_test_tx_btn, FALSE, FALSE, 0);

    return dlg;
}

/* ── config helpers ──────────────────────────────────────────────────── */

std::string rig_config_get_model_id()
{
    if (!g_rig_combo) return "";
    int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_rig_combo));
    if (idx < 0 || idx >= static_cast<int>(g_rig_list.size())) return "";
    return std::to_string(
        static_cast<long>(g_rig_list[static_cast<size_t>(idx)].model_id));
}

std::string rig_config_get_port()
{
    if (!g_port_combo) return "";
    gchar* txt = gtk_combo_box_text_get_active_text(
                     GTK_COMBO_BOX_TEXT(g_port_combo));
    std::string s = txt ? txt : "";
    g_free(txt);
    return s;
}

std::string rig_config_get_baud()
{
    if (!g_baud_combo) return "";
    gchar* txt = gtk_combo_box_text_get_active_text(
                     GTK_COMBO_BOX_TEXT(g_baud_combo));
    std::string s = txt ? txt : "";
    g_free(txt);
    return s;
}

/* ── state queries (for the main-window status line) ────────────────── */

bool        rig_is_connected()      { return g_connected; }
std::string rig_get_current_freq()  { std::lock_guard<std::mutex> lk(g_cache_mutex); return g_cached_freq; }
std::string rig_get_current_mode()  { std::lock_guard<std::mutex> lk(g_cache_mutex); return g_cached_mode; }
bool        rig_get_ptt_on()        { return g_ptt_on; }

void rig_control_set_ptt(bool on)
{
    std::lock_guard<std::mutex> lk(g_rig_mutex);
    if (!g_rig || !g_connected) return;
    rig_set_ptt(g_rig, RIG_VFO_CURR, on ? RIG_PTT_ON : RIG_PTT_OFF);
    g_ptt_on = on;
}

void rig_control_cleanup()
{
    /* Stop the background thread first so it doesn't race with us. */
    g_poll_running.store(false);
    if (g_poll_thread.joinable())
        g_poll_thread.join();

    /* The poll thread may have queued a g_idle_add(update_rig_entries) that
       hasn't run yet.  Null out the widget pointers now so that callback is
       a safe no-op when the main loop eventually dispatches it (the widgets
       will have been destroyed by the time we return to the main loop). */
    g_freq_entry = nullptr;
    g_mode_entry = nullptr;

    std::lock_guard<std::mutex> lk(g_rig_mutex);
    if (!g_rig || !g_connected) return;
    /* Send PTT off, then wait long enough for the slowest CAT rate to flush
       before rig_close() tears down the serial port. */
    rig_set_ptt(g_rig, RIG_VFO_CURR, RIG_PTT_OFF);
    g_ptt_on = false;
    g_usleep(200000);   /* 200 ms — ample for any serial baud rate */
    rig_close(g_rig);
    rig_cleanup(g_rig);
    g_rig = nullptr;
    g_connected = false;
}

/* ────────────────────────────────────────────────────────────────────── */

void rig_config_restore(const std::string& model_id_str,
                        const std::string& port,
                        const std::string& baud)
{
    /* Restore radio model by matching the stored numeric ID */
    if (!model_id_str.empty() && g_rig_combo) {
        rig_model_t mid = static_cast<rig_model_t>(
                              std::atoi(model_id_str.c_str()));
        for (size_t i = 0; i < g_rig_list.size(); ++i) {
            if (g_rig_list[i].model_id == mid) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_rig_combo),
                                         static_cast<int>(i));
                break;
            }
        }
    }

    /* Restore port — try list first, fall back to typing in the entry */
    if (!port.empty() && g_port_combo) {
        bool found = false;
        for (size_t i = 0; i < g_serial_ports.size(); ++i) {
            if (g_serial_ports[i] == port) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_port_combo),
                                         static_cast<int>(i));
                found = true;
                break;
            }
        }
        if (!found) {
            GtkWidget* entry = gtk_bin_get_child(GTK_BIN(g_port_combo));
            if (entry)
                gtk_entry_set_text(GTK_ENTRY(entry), port.c_str());
        }
    }

    /* Restore baud rate */
    static const char* const k_bauds[] = {
        "300", "1200", "2400", "4800", "9600",
        "19200", "38400", "57600", "115200", nullptr
    };
    if (!baud.empty() && g_baud_combo) {
        for (int i = 0; k_bauds[i]; ++i) {
            if (baud == k_bauds[i]) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_baud_combo), i);
                break;
            }
        }
    }
}
