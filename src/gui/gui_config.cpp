#include "gui_config.h"
#include "gui_app_state.h"
#include "rig_control.h"

#include <fstream>
#include <sys/stat.h>

/* ── config persistence ─────────────────────────────────────────────────── */

std::string config_path()
{
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.config";
    mkdir(dir.c_str(), 0755);
    return dir + "/radae-decoder.conf";
}

void save_config()
{
    int in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_input_combo));
    int out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_output_combo));

    std::string in_name, out_name;
    if (in_idx >= 0 && in_idx < static_cast<int>(g_input_devices.size()))
        in_name = g_input_devices[static_cast<size_t>(in_idx)].name;
    if (out_idx >= 0 && out_idx < static_cast<int>(g_output_devices.size()))
        out_name = g_output_devices[static_cast<size_t>(out_idx)].name;

    int tx_in_idx  = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_input_combo));
    int tx_out_idx = gtk_combo_box_get_active(GTK_COMBO_BOX(g_tx_output_combo));

    std::string tx_in_name, tx_out_name;
    if (tx_in_idx >= 0 && tx_in_idx < static_cast<int>(g_tx_input_devices.size()))
        tx_in_name = g_tx_input_devices[static_cast<size_t>(tx_in_idx)].name;
    if (tx_out_idx >= 0 && tx_out_idx < static_cast<int>(g_tx_output_devices.size()))
        tx_out_name = g_tx_output_devices[static_cast<size_t>(tx_out_idx)].name;

    std::ofstream f(config_path());
    if (f) {
        f << "input=" << in_name << '\n';
        f << "output=" << out_name << '\n';
        f << "tx_input=" << tx_in_name << '\n';
        f << "tx_output=" << tx_out_name << '\n';
        f << "tx_level=" << static_cast<int>(gtk_range_get_value(GTK_RANGE(g_tx_slider))) << '\n';
        f << "mic_level=" << static_cast<int>(gtk_range_get_value(GTK_RANGE(g_mic_slider))) << '\n';
        f << "bpf_enabled=" << (g_bpf_switch && gtk_switch_get_active(GTK_SWITCH(g_bpf_switch)) ? 1 : 0) << '\n';
        const char* cs = g_callsign_entry ? gtk_entry_get_text(GTK_ENTRY(g_callsign_entry)) : "";
        f << "callsign=" << cs << '\n';
        const char* gs = g_gridsquare_entry ? gtk_entry_get_text(GTK_ENTRY(g_gridsquare_entry)) : "";
        f << "gridsquare=" << gs << '\n';
        f << "rig_model_id=" << rig_config_get_model_id() << '\n';
        f << "rig_port="     << rig_config_get_port()     << '\n';
        f << "rig_baud="     << rig_config_get_baud()     << '\n';
    }
}

/* Try to select saved devices in the combos.  Returns true if both found. */
bool restore_config()
{
    std::ifstream f(config_path());
    if (!f) return false;

    std::string saved_in, saved_out, saved_tx_in, saved_tx_out, saved_callsign, saved_gridsquare;
    std::string saved_rig_model_id, saved_rig_port, saved_rig_baud;
    int saved_tx_level = -1;
    int saved_mic_level = -1;
    int saved_bpf_enabled = -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, 6, "input=") == 0)
            saved_in = line.substr(6);
        else if (line.compare(0, 7, "output=") == 0)
            saved_out = line.substr(7);
        else if (line.compare(0, 9, "tx_input=") == 0)
            saved_tx_in = line.substr(9);
        else if (line.compare(0, 10, "tx_output=") == 0)
            saved_tx_out = line.substr(10);
        else if (line.compare(0, 9, "tx_level=") == 0)
            saved_tx_level = std::stoi(line.substr(9));
        else if (line.compare(0, 10, "mic_level=") == 0)
            saved_mic_level = std::stoi(line.substr(10));
        else if (line.compare(0, 12, "bpf_enabled=") == 0)
            saved_bpf_enabled = std::stoi(line.substr(12));
        else if (line.compare(0, 9, "callsign=") == 0)
            saved_callsign = line.substr(9);
        else if (line.compare(0, 11, "gridsquare=") == 0)
            saved_gridsquare = line.substr(11);
        else if (line.compare(0, 13, "rig_model_id=") == 0)
            saved_rig_model_id = line.substr(13);
        else if (line.compare(0, 9, "rig_port=") == 0)
            saved_rig_port = line.substr(9);
        else if (line.compare(0, 9, "rig_baud=") == 0)
            saved_rig_baud = line.substr(9);
    }

    /* Restore rig settings unconditionally — must happen before any early return. */
    rig_config_restore(saved_rig_model_id, saved_rig_port, saved_rig_baud);

    if (saved_in.empty() && saved_out.empty()) return false;

    int in_idx = -1, out_idx = -1;
    for (size_t i = 0; i < g_input_devices.size(); i++) {
        if (g_input_devices[i].name == saved_in) {
            in_idx = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < g_output_devices.size(); i++) {
        if (g_output_devices[i].name == saved_out) {
            out_idx = static_cast<int>(i);
            break;
        }
    }

    int tx_in_idx = -1, tx_out_idx = -1;
    for (size_t i = 0; i < g_tx_input_devices.size(); i++) {
        if (g_tx_input_devices[i].name == saved_tx_in) {
            tx_in_idx = static_cast<int>(i);
            break;
        }
    }
    for (size_t i = 0; i < g_tx_output_devices.size(); i++) {
        if (g_tx_output_devices[i].name == saved_tx_out) {
            tx_out_idx = static_cast<int>(i);
            break;
        }
    }

    g_updating_combos = true;
    if (in_idx >= 0)     gtk_combo_box_set_active(GTK_COMBO_BOX(g_input_combo), in_idx);
    if (out_idx >= 0)    gtk_combo_box_set_active(GTK_COMBO_BOX(g_output_combo), out_idx);
    if (tx_in_idx >= 0)  gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_input_combo), tx_in_idx);
    if (tx_out_idx >= 0) gtk_combo_box_set_active(GTK_COMBO_BOX(g_tx_output_combo), tx_out_idx);
    g_updating_combos = false;

    if (saved_tx_level >= 0 && saved_tx_level <= 100)
        gtk_range_set_value(GTK_RANGE(g_tx_slider), saved_tx_level);
    if (saved_mic_level >= 0 && saved_mic_level <= 100)
        gtk_range_set_value(GTK_RANGE(g_mic_slider), saved_mic_level);
    if (saved_bpf_enabled >= 0 && g_bpf_switch)
        gtk_switch_set_active(GTK_SWITCH(g_bpf_switch), saved_bpf_enabled != 0);
    if (!saved_callsign.empty() && g_callsign_entry)
        gtk_entry_set_text(GTK_ENTRY(g_callsign_entry), saved_callsign.c_str());
    if (!saved_gridsquare.empty() && g_gridsquare_entry)
        gtk_entry_set_text(GTK_ENTRY(g_gridsquare_entry), saved_gridsquare.c_str());

    return (in_idx >= 0 && out_idx >= 0);
}
