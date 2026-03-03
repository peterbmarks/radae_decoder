#include "rig_control.h"

#include <gtk/gtk.h>
#include <hamlib/rig.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <glob.h>
#include <mutex>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <string>
#include <thread>
#include <unistd.h>
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
    glob_add("/dev/tty.*");
}

/* ── widget globals ──────────────────────────────────────────────────── */

static GtkWidget* g_rig_combo      = nullptr;
static GtkWidget* g_conn_type_combo = nullptr;
static GtkWidget* g_port_combo     = nullptr;
static GtkWidget* g_baud_combo     = nullptr;
static GtkWidget* g_tcp_host_entry = nullptr;
static GtkWidget* g_tcp_port_entry = nullptr;
static GtkWidget* g_serial_port_row = nullptr;
static GtkWidget* g_baud_row = nullptr;
static GtkWidget* g_tcp_host_row = nullptr;
static GtkWidget* g_tcp_port_row = nullptr;
static GtkWidget* g_connect_btn    = nullptr;
static GtkWidget* g_status_lbl     = nullptr;
static GtkWidget* g_freq_entry     = nullptr;
static GtkWidget* g_mode_entry     = nullptr;
static GtkWidget* g_test_tx_btn    = nullptr;

static RIG*  g_rig       = nullptr;
static bool  g_connected = false;
static bool  g_ptt_on    = false;
static bool  g_ptt_supported = true;
static bool  g_conn_params_valid = false;
static rig_model_t g_conn_model_id = RIG_MODEL_NONE;
static bool  g_conn_use_tcp = false;
static std::string g_conn_serial_path;
static int   g_conn_baud = 9600;
static std::string g_conn_tcp_host;
static int   g_conn_tcp_port = 0;

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

static int find_model_index(rig_model_t model_id)
{
    for (size_t i = 0; i < g_rig_list.size(); ++i) {
        if (g_rig_list[i].model_id == model_id)
            return static_cast<int>(i);
    }
    return -1;
}

/* ── helpers ─────────────────────────────────────────────────────────── */

static void set_status(const char* msg)
{
    if (g_status_lbl)
        gtk_label_set_text(GTK_LABEL(g_status_lbl), msg);
}

static std::string trim_copy(const std::string& s)
{
    size_t b = 0;
    while (b < s.size() &&
           std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    size_t e = s.size();
    while (e > b &&
           std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

static std::string get_combo_text(GtkWidget* combo)
{
    if (!combo) return "";

    gchar* txt = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (txt) {
        std::string out = txt;
        g_free(txt);
        return out;
    }

    /* For combo-with-entry when typed text has no active list item. */
    GtkWidget* child = gtk_bin_get_child(GTK_BIN(combo));
    if (child && GTK_IS_ENTRY(child)) {
        const char* et = gtk_entry_get_text(GTK_ENTRY(child));
        return et ? et : "";
    }
    return "";
}

static bool starts_with_tcp_uri(const std::string& s)
{
    if (s.size() < 6) return false;
    return std::tolower(static_cast<unsigned char>(s[0])) == 't' &&
           std::tolower(static_cast<unsigned char>(s[1])) == 'c' &&
           std::tolower(static_cast<unsigned char>(s[2])) == 'p' &&
           s[3] == ':' && s[4] == '/' && s[5] == '/';
}

static bool parse_tcp_port(const std::string& s, int& out_port)
{
    const std::string p = trim_copy(s);
    if (p.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(p.c_str(), &end, 10);
    if (!end || *end != '\0' || v < 1 || v > 65535) return false;
    out_port = static_cast<int>(v);
    return true;
}

static bool parse_tcp_uri(const std::string& raw_uri, std::string& host, int& port)
{
    std::string s = trim_copy(raw_uri);
    if (!starts_with_tcp_uri(s)) return false;
    s = s.substr(6); /* strip tcp:// */

    size_t colon = s.rfind(':');
    if (colon == std::string::npos) return false;

    host = trim_copy(s.substr(0, colon));
    if (host.empty()) return false;
    return parse_tcp_port(s.substr(colon + 1), port);
}

static bool is_tcp_mode_selected()
{
    return g_conn_type_combo &&
           gtk_combo_box_get_active(GTK_COMBO_BOX(g_conn_type_combo)) == 1;
}

static void update_connection_mode_ui()
{
    const bool tcp = is_tcp_mode_selected();
    const bool editable = !g_connected;

    if (g_serial_port_row) gtk_widget_set_visible(g_serial_port_row, !tcp);
    if (g_baud_row)        gtk_widget_set_visible(g_baud_row,        !tcp);
    if (g_tcp_host_row)    gtk_widget_set_visible(g_tcp_host_row,     tcp);
    if (g_tcp_port_row)    gtk_widget_set_visible(g_tcp_port_row,     tcp);

    if (g_port_combo)      gtk_widget_set_sensitive(g_port_combo,      editable && !tcp);
    if (g_baud_combo)      gtk_widget_set_sensitive(g_baud_combo,      editable && !tcp);
    if (g_tcp_host_entry)  gtk_widget_set_sensitive(g_tcp_host_entry,  editable && tcp);
    if (g_tcp_port_entry)  gtk_widget_set_sensitive(g_tcp_port_entry,  editable && tcp);
}

static void set_combo_with_entry_text(GtkWidget* combo,
                                      const std::vector<std::string>& choices,
                                      const std::string& value)
{
    if (!combo || value.empty()) return;

    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i] == value) {
            gtk_combo_box_set_active(GTK_COMBO_BOX(combo), static_cast<int>(i));
            return;
        }
    }

    GtkWidget* entry = gtk_bin_get_child(GTK_BIN(combo));
    if (entry && GTK_IS_ENTRY(entry))
        gtk_entry_set_text(GTK_ENTRY(entry), value.c_str());
}

static void on_connection_type_changed(GtkComboBox* /*box*/, gpointer /*data*/)
{
    update_connection_mode_ui();
    fire_save();
}

static void configure_rig_transport(RIG* rig,
                                    bool use_tcp,
                                    const std::string& serial_path,
                                    int baud,
                                    const std::string& tcp_host,
                                    int tcp_port)
{
    if (!rig) return;
    if (use_tcp) {
        rig->state.rigport.type.rig = RIG_PORT_NETWORK;
        const std::string endpoint = tcp_host + ":" + std::to_string(tcp_port);
        std::strncpy(rig->state.rigport.pathname, endpoint.c_str(),
                     HAMLIB_FILPATHLEN - 1);
        rig->state.rigport.client_port = tcp_port;
    } else {
        rig->state.rigport.type.rig = RIG_PORT_SERIAL;
        std::strncpy(rig->state.rigport.pathname, serial_path.c_str(),
                     HAMLIB_FILPATHLEN - 1);
        rig->state.rigport.parm.serial.rate = baud;
    }
    rig->state.rigport.pathname[HAMLIB_FILPATHLEN - 1] = '\0';
}

static void force_netrigctl_ptt_rig(RIG* rig, rig_model_t model_id, bool use_tcp)
{
    if (!rig || !use_tcp || model_id != RIG_MODEL_NETRIGCTL) return;
    /* Avoid MIC/DATA mode ambiguity on some RigCtl servers. */
    rig->state.ptt_type = RIG_PTT_RIG;
    rig->state.pttport.type.ptt = RIG_PTT_RIG;
}

static bool allow_data_ptt_fallback(rig_model_t model_id, bool use_tcp)
{
    /* netrigctl/rigctld endpoints generally accept only T 0 / T 1. */
    return !(use_tcp && model_id == RIG_MODEL_NETRIGCTL);
}

static std::string first_response_line(const std::string& response)
{
    size_t end = response.find_first_of("\r\n");
    if (end == std::string::npos)
        return trim_copy(response);
    return trim_copy(response.substr(0, end));
}

static bool rigctl_response_ok(const std::string& response)
{
    const std::string line = first_response_line(response);
    if (line.empty()) return false;
    return line == "0" ||
           line == "RPRT 0" ||
           line.find("RPRT 0") != std::string::npos;
}

static bool tcp_cat_command(const std::string& host,
                            int tcp_port,
                            const std::string& command,
                            std::string* response_out = nullptr,
                            std::string* err_out = nullptr)
{
    if (response_out) response_out->clear();
    if (err_out) err_out->clear();
    if (host.empty() || tcp_port < 1 || tcp_port > 65535) {
        if (err_out) *err_out = "Invalid TCP CAT endpoint.";
        return false;
    }

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char port_buf[16];
    std::snprintf(port_buf, sizeof port_buf, "%d", tcp_port);

    struct addrinfo* addrs = nullptr;
    int gai = getaddrinfo(host.c_str(), port_buf, &hints, &addrs);
    if (gai != 0 || !addrs) {
        if (err_out)
            *err_out = std::string("TCP CAT resolve failed: ") + gai_strerror(gai);
        return false;
    }

    std::string wire = command;
    if (wire.empty() || wire.back() != '\n')
        wire.push_back('\n');

    bool ok = false;
    std::string last_err = "TCP CAT connect failed.";

    for (struct addrinfo* ai = addrs; ai; ai = ai->ai_next) {
        int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
            ::close(fd);
            continue;
        }

        size_t sent = 0;
        while (sent < wire.size()) {
            ssize_t n = ::send(fd, wire.data() + sent, wire.size() - sent, 0);
            if (n <= 0) {
                last_err = "TCP CAT send failed.";
                break;
            }
            sent += static_cast<size_t>(n);
        }

        std::string response;
        if (sent == wire.size()) {
            char buf[256];
            while (response.size() < 2048) {
                ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                response.append(buf, static_cast<size_t>(n));
                if (response.find('\n') != std::string::npos) break;
            }
            if (response.empty())
                last_err = "TCP CAT no response.";
        }

        ::close(fd);

        if (sent == wire.size() && !response.empty()) {
            if (response_out) *response_out = response;
            ok = true;
            break;
        }
    }

    freeaddrinfo(addrs);

    if (!ok && err_out && err_out->empty())
        *err_out = last_err;
    return ok;
}

static bool set_ptt_via_tcp_cat_raw(bool on, std::string* err_out = nullptr)
{
    if (err_out) err_out->clear();
    if (!g_conn_params_valid || !g_conn_use_tcp || g_conn_model_id != RIG_MODEL_NETRIGCTL)
        return false;

    auto try_set = [&](int ptt_val, std::string& cmd_err) -> bool {
        std::string resp;
        if (!tcp_cat_command(g_conn_tcp_host, g_conn_tcp_port,
                             "T " + std::to_string(ptt_val), &resp, &cmd_err))
            return false;
        if (!rigctl_response_ok(resp)) {
            cmd_err = "TCP CAT rejected command: " + first_response_line(resp);
            return false;
        }
        return true;
    };

    std::string cmd_err;
    if (!on) {
        if (!try_set(0, cmd_err)) {
            if (err_out) *err_out = cmd_err;
            return false;
        }
        return true;
    }

    const int tries[] = { 1, 3 };
    for (int ptt_val : tries) {
        cmd_err.clear();
        if (!try_set(ptt_val, cmd_err))
            continue;
        return true;
    }

    if (err_out) {
        if (!cmd_err.empty()) *err_out = cmd_err;
        else *err_out = "TCP CAT PTT set failed.";
    }
    return false;
}

static int try_open_model(rig_model_t model_id,
                          bool use_tcp,
                          const std::string& serial_path,
                          int baud,
                          const std::string& tcp_host,
                          int tcp_port,
                          std::string& err_out)
{
    RIG* rig = rig_init(model_id);
    if (!rig) {
        err_out = "rig_init() failed — invalid model?";
        return -RIG_EINVAL;
    }

    configure_rig_transport(rig, use_tcp, serial_path, baud, tcp_host, tcp_port);

    int ret = rig_open(rig);
    if (ret != RIG_OK) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "rig_open: %s", rigerror(ret));
        err_out = buf;
        rig_cleanup(rig);
        return ret;
    }

    force_netrigctl_ptt_rig(rig, model_id, use_tcp);
    g_rig = rig;
    err_out.clear();
    return RIG_OK;
}

static bool is_definitely_unsupported_ptt_error(int ret)
{
    return ret == -RIG_EINVAL ||
           ret == -RIG_ENIMPL ||
           ret == -RIG_ENAVAIL ||
           ret == -RIG_ECONF;
}

static bool set_ptt_via_fresh_connection(bool on, std::string* err_out = nullptr)
{
    if (err_out) err_out->clear();
    if (!g_conn_params_valid) return false;
    const bool allow_data = allow_data_ptt_fallback(g_conn_model_id, g_conn_use_tcp);

    RIG* rig = rig_init(g_conn_model_id);
    if (!rig) {
        if (err_out) *err_out = "rig_init failed for fresh PTT connection.";
        return false;
    }

    configure_rig_transport(rig, g_conn_use_tcp, g_conn_serial_path, g_conn_baud,
                            g_conn_tcp_host, g_conn_tcp_port);
    int ret = rig_open(rig);
    if (ret != RIG_OK) {
        if (err_out) *err_out = std::string("fresh rig_open: ") + rigerror(ret);
        rig_cleanup(rig);
        return false;
    }

    force_netrigctl_ptt_rig(rig, g_conn_model_id, g_conn_use_tcp);

    bool ok = false;
    int ptt_ret = -RIG_EIO;
    if (!on) {
        ptt_ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_OFF);
        if (ptt_ret == RIG_OK)
            ok = true;
    } else {
        ptt_ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_ON);
        if (ptt_ret != RIG_OK && allow_data)
            ptt_ret = rig_set_ptt(rig, RIG_VFO_CURR, RIG_PTT_ON_DATA);
        if (ptt_ret == RIG_OK)
            ok = true;
    }

    if (!ok) {
        ptt_t cur = RIG_PTT_OFF;
        if (rig_get_ptt(rig, RIG_VFO_CURR, &cur) == RIG_OK) {
            if (!on && cur == RIG_PTT_OFF) ok = true;
            if (on && cur != RIG_PTT_OFF) ok = true;
        }
    }

    if (!ok && err_out)
        *err_out = std::string("fresh PTT failed: ") + rigerror(ptt_ret);

    rig_close(rig);
    rig_cleanup(rig);
    return ok;
}

static bool set_ptt_with_retry(bool on,
                               bool* unsupported_out = nullptr,
                               std::string* err_out = nullptr)
{
    if (unsupported_out) *unsupported_out = false;
    if (err_out) err_out->clear();
    if (!g_rig || !g_connected) return false;

    const ptt_t desired = on ? RIG_PTT_ON : RIG_PTT_OFF;
    int last_ret = -RIG_EIO;
    ptt_t desired_sent = desired;
    rig_model_t connected_model = g_conn_model_id;
    if (g_rig && g_rig->caps)
        connected_model = g_rig->caps->rig_model;
    const bool prefer_raw_tcp = g_conn_params_valid &&
                                g_conn_use_tcp &&
                                connected_model == RIG_MODEL_NETRIGCTL;
    const bool allow_data = allow_data_ptt_fallback(connected_model, g_conn_use_tcp);
    bool tried_raw_early = false;

    if (prefer_raw_tcp) {
        tried_raw_early = true;
        std::string raw_err;
        if (set_ptt_via_tcp_cat_raw(on, &raw_err)) {
            g_ptt_on = on;
            return true;
        }
        if (err_out && !raw_err.empty())
            *err_out = raw_err;
    }

    const int max_tries = prefer_raw_tcp ? 1 : 3;
    const guint backoff_us = prefer_raw_tcp ? 30000 : 80000;

    for (int i = 0; i < max_tries; ++i) {
        if (!on) {
            desired_sent = RIG_PTT_OFF;
            last_ret = rig_set_ptt(g_rig, RIG_VFO_CURR, desired_sent);
            if (last_ret == RIG_OK) {
                g_ptt_on = false;
                return true;
            }
        } else {
            desired_sent = RIG_PTT_ON;
            last_ret = rig_set_ptt(g_rig, RIG_VFO_CURR, desired_sent);
            if (last_ret != RIG_OK && allow_data) {
                desired_sent = RIG_PTT_ON_DATA;
                last_ret = rig_set_ptt(g_rig, RIG_VFO_CURR, desired_sent);
            }
            if (last_ret == RIG_OK) {
                g_ptt_on = true;
                return true;
            }
        }

        /* Some endpoints may switch state but fail to return cleanly. */
        ptt_t cur = RIG_PTT_OFF;
        if (rig_get_ptt(g_rig, RIG_VFO_CURR, &cur) == RIG_OK) {
            if (!on && cur == RIG_PTT_OFF) {
                g_ptt_on = false;
                return true;
            }
            if (on && cur != RIG_PTT_OFF) {
                g_ptt_on = true;
                return true;
            }
        }

        g_usleep(backoff_us);
    }

    /* Fallback for flaky long-lived TCP CAT sessions:
       retry the PTT command using a short-lived fresh connection. */
    if (g_conn_params_valid && g_conn_use_tcp) {
        std::string fresh_err;
        if (set_ptt_via_fresh_connection(on, &fresh_err)) {
            g_ptt_on = on;
            return true;
        }
        if (err_out && !fresh_err.empty())
            *err_out = fresh_err;
    }

    /* Last fallback for netrigctl endpoints:
       send raw rigctld command over TCP to bypass Hamlib state/lock issues. */
    if (!tried_raw_early &&
        g_conn_params_valid &&
        g_conn_use_tcp &&
        connected_model == RIG_MODEL_NETRIGCTL) {
        std::string raw_err;
        if (set_ptt_via_tcp_cat_raw(on, &raw_err)) {
            g_ptt_on = on;
            return true;
        }
        if (err_out && err_out->empty() && !raw_err.empty())
            *err_out = raw_err;
    }

    if (unsupported_out)
        *unsupported_out = is_definitely_unsupported_ptt_error(last_ret);
    if (err_out)
        if (err_out->empty()) *err_out = rigerror(last_ret);
    return false;
}

static void apply_connected_state(bool connected)
{
    g_connected = connected;
    gtk_button_set_label(GTK_BUTTON(g_connect_btn),
                         connected ? "Disconnect" : "Connect");
    gtk_widget_set_sensitive(g_test_tx_btn, connected && g_ptt_supported);
    gtk_widget_set_sensitive(g_rig_combo,   !connected);
    gtk_widget_set_sensitive(g_conn_type_combo, !connected);
    update_connection_mode_ui();
    if (!connected) {
        g_cached_freq.clear();
        g_cached_mode.clear();
        g_ptt_on = false;
        g_ptt_supported = true;
        g_conn_params_valid = false;
        g_conn_model_id = RIG_MODEL_NONE;
        g_conn_use_tcp = false;
        g_conn_serial_path.clear();
        g_conn_baud = 9600;
        g_conn_tcp_host.clear();
        g_conn_tcp_port = 0;
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

    bool use_tcp = is_tcp_mode_selected();
    std::string serial_path;
    int baud = 9600;
    std::string tcp_host;
    int tcp_port = 0;

    if (use_tcp) {
        tcp_host = trim_copy(g_tcp_host_entry
                               ? gtk_entry_get_text(GTK_ENTRY(g_tcp_host_entry))
                               : "");
        if (tcp_host.empty())
            return "Enter a TCP host first.";
        if (!parse_tcp_port(g_tcp_port_entry
                              ? gtk_entry_get_text(GTK_ENTRY(g_tcp_port_entry))
                              : "", tcp_port)) {
            return "TCP port must be a number between 1 and 65535.";
        }
    } else {
        serial_path = trim_copy(get_combo_text(g_port_combo));
        if (serial_path.empty())
            return "Select a serial port first.";

        /* Backward-compatible: allow saved/manual tcp://host:port text in port. */
        if (starts_with_tcp_uri(serial_path)) {
            use_tcp = parse_tcp_uri(serial_path, tcp_host, tcp_port);
            if (!use_tcp)
                return "Invalid TCP CAT endpoint. Use tcp://host:port.";
        } else {
            const std::string baud_str = get_combo_text(g_baud_combo);
            if (!baud_str.empty())
                baud = std::atoi(baud_str.c_str());
        }
    }

    rig_model_t model_id = g_rig_list[static_cast<size_t>(model_idx)].model_id;
    std::string err;
    int ret = try_open_model(model_id, use_tcp,
                             serial_path, baud, tcp_host, tcp_port, err);

    /* Common case with SmartSDR CAT / rigctld endpoints:
       user picks a vendor-specific TCP backend, but endpoint is RigCtl protocol. */
    if (ret != RIG_OK && use_tcp && model_id != RIG_MODEL_NETRIGCTL) {
        std::string fallback_err;
        int ret2 = try_open_model(RIG_MODEL_NETRIGCTL, true,
                                  serial_path, baud, tcp_host, tcp_port, fallback_err);
        if (ret2 == RIG_OK) {
            int net_idx = find_model_index(RIG_MODEL_NETRIGCTL);
            if (net_idx >= 0 && g_rig_combo)
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_rig_combo), net_idx);
            fire_save();
        } else {
            err += "  Hint: for RigCtl/Hamlib TCP servers, choose model \"Hamlib  NET rigctl\".";
        }
    }

    if (!g_rig)
        return err.empty() ? "rig_open failed." : err;

    rig_model_t connected_model = model_id;
    if (g_rig->caps)
        connected_model = g_rig->caps->rig_model;

    g_conn_params_valid = true;
    g_conn_model_id = connected_model;
    g_conn_use_tcp = use_tcp;
    g_conn_serial_path = serial_path;
    g_conn_baud = baud;
    g_conn_tcp_host = tcp_host;
    g_conn_tcp_port = tcp_port;

    g_ptt_supported = true;
    g_ptt_on = false;
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
    if (g_rig && g_connected) {
        std::lock_guard<std::mutex> lk(g_rig_mutex);
        bool unsupported = false;
        std::string err;
        if (!set_ptt_with_retry(false, &unsupported, &err) && unsupported) {
            g_ptt_supported = false;
            if (g_test_tx_btn) gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
            set_status("Connected. CAT endpoint does not support PTT set; use VOX or external PTT.");
        }
    }
    g_ptt_on = false;
    gtk_widget_set_sensitive(g_test_tx_btn, g_ptt_supported ? TRUE : FALSE);
    gtk_button_set_label(GTK_BUTTON(g_test_tx_btn), "Test TX (1 s)");
    if (g_ptt_supported)
        set_status("Connected.  PTT test complete.");
    return FALSE; /* one-shot */
}

static void on_test_tx(GtkButton* /*btn*/, gpointer /*data*/)
{
    if (!g_rig || !g_connected) { set_status("Not connected."); return; }
    if (!g_ptt_supported) {
        set_status("Connected. CAT endpoint does not support PTT set; use VOX or external PTT.");
        return;
    }

    std::lock_guard<std::mutex> lk(g_rig_mutex);
    bool unsupported = false;
    std::string err;
    if (!set_ptt_with_retry(true, &unsupported, &err)) {
        if (unsupported) {
            g_ptt_supported = false;
            gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "CAT PTT unsupported (%s). Use VOX or external PTT.", err.c_str());
            set_status(buf);
        } else {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "PTT command failed (%s). Retry or use VOX/external PTT.", err.c_str());
            set_status(buf);
        }
        return;
    }

    gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
    gtk_button_set_label(GTK_BUTTON(g_test_tx_btn), "TX\xe2\x80\xa6");  /* TX… */
    set_status("PTT ON \xe2\x80\x94 will turn off in 1 s\xe2\x80\xa6");
    g_timeout_add(1000, on_ptt_off, nullptr);
}

/* ── auto-connect at launch ──────────────────────────────────────────── */

void rig_auto_connect(GtkWindow* parent)
{
    /* Only attempt if both a model and connection endpoint are restored. */
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(g_rig_combo)) < 0)
        return;

    bool ready = false;
    if (is_tcp_mode_selected()) {
        const std::string host = trim_copy(g_tcp_host_entry
                                             ? gtk_entry_get_text(GTK_ENTRY(g_tcp_host_entry))
                                             : "");
        int tcp_port = 0;
        ready = !host.empty() &&
                parse_tcp_port(g_tcp_port_entry
                                 ? gtk_entry_get_text(GTK_ENTRY(g_tcp_port_entry))
                                 : "", tcp_port);
    } else {
        ready = !trim_copy(get_combo_text(g_port_combo)).empty();
    }
    if (!ready) return;

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
                      "%s", hamlib_version2);
        GtkWidget* lbl = gtk_label_new(nullptr);
        gtk_label_set_markup(GTK_LABEL(lbl), buf);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(content),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* helper: build a label + widget row and pack it */
    auto add_row = [&](const char* label_text, GtkWidget* widget) -> GtkWidget* {
        GtkWidget* hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget* lbl  = gtk_label_new(label_text);
        gtk_widget_set_size_request(lbl, 90, -1);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_box_pack_start(GTK_BOX(hbox), lbl,    FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE,  TRUE,  0);
        gtk_box_pack_start(GTK_BOX(content), hbox, FALSE, FALSE, 0);
        return hbox;
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

    /* ── connection type (Serial CAT / TCP CAT) ─────────────────── */
    g_conn_type_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_conn_type_combo), "Serial CAT");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_conn_type_combo), "TCP CAT");
    gtk_combo_box_set_active(GTK_COMBO_BOX(g_conn_type_combo), 0);
    gtk_widget_set_tooltip_text(g_conn_type_combo,
        "Choose how to connect CAT control: serial cable or TCP socket");
    g_signal_connect(g_conn_type_combo, "changed",
                     G_CALLBACK(on_connection_type_changed), nullptr);
    add_row("Connection:", g_conn_type_combo);

    /* ── serial port combo (with editable entry for custom paths) ── */
    g_port_combo = gtk_combo_box_text_new_with_entry();
    for (const auto& p : g_serial_ports)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g_port_combo),
                                       p.c_str());
    gtk_widget_set_tooltip_text(g_port_combo,
        "Serial port connected to the rig (you can also type a path)");
    g_signal_connect_swapped(g_port_combo, "changed",
                             G_CALLBACK(fire_save), nullptr);
    g_serial_port_row = add_row("Port:", g_port_combo);
    if (GtkWidget* entry = gtk_bin_get_child(GTK_BIN(g_port_combo)); entry && GTK_IS_ENTRY(entry))
        g_signal_connect_swapped(entry, "changed", G_CALLBACK(fire_save), nullptr);

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
    g_baud_row = add_row("Baud rate:", g_baud_combo);

    /* ── TCP host / port fields ──────────────────────────────────── */
    g_tcp_host_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_tcp_host_entry), "127.0.0.1");
    gtk_widget_set_tooltip_text(g_tcp_host_entry,
        "TCP hostname or IP for CAT server (for example: 192.168.1.25)");
    g_signal_connect_swapped(g_tcp_host_entry, "changed",
                             G_CALLBACK(fire_save), nullptr);
    g_tcp_host_row = add_row("TCP Host:", g_tcp_host_entry);

    g_tcp_port_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_tcp_port_entry), "4532");
    gtk_widget_set_tooltip_text(g_tcp_port_entry,
        "TCP CAT port (for example rigctld default: 4532)");
    g_signal_connect_swapped(g_tcp_port_entry, "changed",
                             G_CALLBACK(fire_save), nullptr);
    g_tcp_port_row = add_row("TCP Port:", g_tcp_port_entry);

    update_connection_mode_ui();

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
    if (is_tcp_mode_selected()) {
        const std::string host = trim_copy(g_tcp_host_entry
                                             ? gtk_entry_get_text(GTK_ENTRY(g_tcp_host_entry))
                                             : "");
        if (host.empty()) return "";
        std::string port = trim_copy(g_tcp_port_entry
                                       ? gtk_entry_get_text(GTK_ENTRY(g_tcp_port_entry))
                                       : "");
        if (port.empty()) port = "4532";
        return "tcp://" + host + ":" + port;
    }
    return trim_copy(get_combo_text(g_port_combo));
}

std::string rig_config_get_baud()
{
    if (is_tcp_mode_selected()) return "";
    if (!g_baud_combo) return "";
    return trim_copy(get_combo_text(g_baud_combo));
}

/* ── state queries (for the main-window status line) ────────────────── */

bool        rig_is_connected()      { return g_connected; }
bool        rig_is_ptt_supported()  { return g_ptt_supported; }
std::string rig_get_current_freq()  { std::lock_guard<std::mutex> lk(g_cache_mutex); return g_cached_freq; }
std::string rig_get_current_mode()  { std::lock_guard<std::mutex> lk(g_cache_mutex); return g_cached_mode; }
bool        rig_get_ptt_on()        { return g_ptt_on; }

void rig_control_set_ptt(bool on)
{
    std::lock_guard<std::mutex> lk(g_rig_mutex);
    if (!g_rig || !g_connected || !g_ptt_supported) return;
    bool unsupported = false;
    std::string err;
    if (!set_ptt_with_retry(on, &unsupported, &err)) {
        if (unsupported) {
            g_ptt_supported = false;
            if (g_test_tx_btn) gtk_widget_set_sensitive(g_test_tx_btn, FALSE);
        }
        if (on) g_ptt_on = false;
        return;
    }
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

    bool restored_tcp = false;
    if (!port.empty() && starts_with_tcp_uri(port)) {
        restored_tcp = true;
        if (g_conn_type_combo)
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_conn_type_combo), 1);

        std::string host;
        int tcp_port = 0;
        if (parse_tcp_uri(port, host, tcp_port)) {
            if (g_tcp_host_entry)
                gtk_entry_set_text(GTK_ENTRY(g_tcp_host_entry), host.c_str());
            if (g_tcp_port_entry) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "%d", tcp_port);
                gtk_entry_set_text(GTK_ENTRY(g_tcp_port_entry), buf);
            }
        } else {
            /* Preserve malformed tcp:// text for user correction. */
            std::string s = port.substr(6);
            size_t colon = s.rfind(':');
            std::string raw_host = (colon == std::string::npos) ? s : s.substr(0, colon);
            std::string raw_port = (colon == std::string::npos) ? "" : s.substr(colon + 1);
            if (g_tcp_host_entry)
                gtk_entry_set_text(GTK_ENTRY(g_tcp_host_entry), raw_host.c_str());
            if (g_tcp_port_entry)
                gtk_entry_set_text(GTK_ENTRY(g_tcp_port_entry), raw_port.c_str());
        }
    } else {
        if (g_conn_type_combo)
            gtk_combo_box_set_active(GTK_COMBO_BOX(g_conn_type_combo), 0);
        set_combo_with_entry_text(g_port_combo, g_serial_ports, port);
    }

    /* Restore baud rate (serial mode only) */
    static const char* const k_bauds[] = {
        "300", "1200", "2400", "4800", "9600",
        "19200", "38400", "57600", "115200", nullptr
    };
    if (!restored_tcp && !baud.empty() && g_baud_combo) {
        for (int i = 0; k_bauds[i]; ++i) {
            if (baud == k_bauds[i]) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(g_baud_combo), i);
                break;
            }
        }
    }

    update_connection_mode_ui();
}
