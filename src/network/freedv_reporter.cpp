#include "freedv_reporter.h"

#include "yyjson.h"

#include <algorithm>
#include <iostream>
#include <string>

// ─── Platform string ──────────────────────────────────────────────────────────
#ifdef __APPLE__
static constexpr const char* kOs = "macOS";
#else
static constexpr const char* kOs = "Linux";
#endif

// ─── JSON read helpers ────────────────────────────────────────────────────────
// These operate on a parsed yyjson_val* object and extract typed fields.

static std::string jStr(yyjson_val* obj, const char* key)
{
    yyjson_val* v = yyjson_obj_get(obj, key);
    const char* s = (v && yyjson_is_str(v)) ? yyjson_get_str(v) : nullptr;
    return s ? s : "";
}

static uint64_t jUint(yyjson_val* obj, const char* key)
{
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v) return 0;
    if (yyjson_is_uint(v)) return yyjson_get_uint(v);
    if (yyjson_is_int(v))  return static_cast<uint64_t>(yyjson_get_int(v));
    if (yyjson_is_real(v)) return static_cast<uint64_t>(yyjson_get_real(v));
    return 0;
}

static bool jBool(yyjson_val* obj, const char* key)
{
    yyjson_val* v = yyjson_obj_get(obj, key);
    return (v && yyjson_is_bool(v)) ? yyjson_get_bool(v) : false;
}

static double jReal(yyjson_val* obj, const char* key)
{
    yyjson_val* v = yyjson_obj_get(obj, key);
    if (!v) return 0.0;
    if (yyjson_is_real(v)) return yyjson_get_real(v);
    if (yyjson_is_uint(v)) return static_cast<double>(yyjson_get_uint(v));
    if (yyjson_is_int(v))  return static_cast<double>(yyjson_get_int(v));
    return 0.0;
}

// ─── JSON write helper ────────────────────────────────────────────────────────
// Escape a C++ string for safe embedding inside a JSON string literal.

static std::string jEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        if      (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n";  }
        else if (c == '\r') { out += "\\r";  }
        else if (c == '\t') { out += "\\t";  }
        else if (c < 0x20)  { /* skip control characters */ }
        else                { out += static_cast<char>(c); }
    }
    return out;
}

// ─── FreeDVReporter ───────────────────────────────────────────────────────────

FreeDVReporter::FreeDVReporter(const std::string& callsign,
                               const std::string& gridSquare,
                               const std::string& version,
                               bool               rxOnly,
                               bool               writeOnly,
                               const std::string& host,
                               int                port)
    : callsign_(callsign)
    , gridSquare_(gridSquare.substr(0, std::min(gridSquare.size(), size_t(6))))
    , version_(version)
    , rxOnly_(rxOnly)
    , writeOnly_(writeOnly)
    , host_(host)
    , port_(port)
{
    // Callsign is always sent upper-case.
    std::transform(callsign_.begin(), callsign_.end(),
                   callsign_.begin(), ::toupper);

    // ── Register server → client event handlers ───────────────────────────
    sio_.on("connection_successful",
        [this](const std::string& d) { onConnectionSuccessful(d); });
    sio_.on("new_connection",
        [this](const std::string& d) { onNewConnection(d); });
    sio_.on("remove_connection",
        [this](const std::string& d) { onRemoveConnection(d); });
    sio_.on("freq_change",
        [this](const std::string& d) { onFreqChange(d); });
    sio_.on("tx_report",
        [this](const std::string& d) { onTxReport(d); });
    sio_.on("rx_report",
        [this](const std::string& d) { onRxReport(d); });
    sio_.on("message_update",
        [this](const std::string& d) { onMessageUpdate(d); });
    sio_.on("qsy_request",
        [this](const std::string& d) { onQsyRequest(d); });
    sio_.on("bulk_update",
        [this](const std::string& d) { onBulkUpdate(d); });

    sio_.onDisconnect([this]() {
        fullyConnected_.store(false);
        {
            std::lock_guard<std::mutex> lock(stationMutex_);
            stations_.clear();
        }
        if (stationUpdateCb_) stationUpdateCb_();
    });
}

FreeDVReporter::~FreeDVReporter()
{
    disconnect();
}

// ─── IReporter ────────────────────────────────────────────────────────────────

void FreeDVReporter::freqChange(uint64_t frequency)
{
    localFreq_ = frequency;
    if (!fullyConnected_.load()) return;

    const std::string json = "{\"freq\":" + std::to_string(frequency) + "}";
    sio_.emit("freq_change", json);
}

void FreeDVReporter::transmit(std::string mode, bool tx)
{
    localMode_         = mode;
    localTransmitting_ = tx;
    if (!fullyConnected_.load()) return;

    const std::string json =
        "{\"mode\":\""       + jEscape(mode) + "\","
        "\"transmitting\":"  + (tx ? "true" : "false") + "}";
    sio_.emit("tx_report", json);
}

void FreeDVReporter::inAnalogMode(bool inAnalog)
{
    inAnalog_.store(inAnalog);
    if (!fullyConnected_.load()) return;

    if (inAnalog) {
        sio_.emit("hide_self");
    } else {
        sio_.emit("show_self");
        sendInitialState();
    }
}

void FreeDVReporter::addReceiveRecord(std::string callsign,
                                      std::string mode,
                                      uint64_t    /* frequency */,
                                      signed char snr)
{
    // The API doc says rx_report is only sent after connection_successful.
    if (!fullyConnected_.load()) return;

    const std::string json =
        "{\"callsign\":\""  + jEscape(callsign) + "\","
        "\"mode\":\""       + jEscape(mode)     + "\","
        "\"snr\":"          + std::to_string(static_cast<int>(snr)) + "}";
    sio_.emit("rx_report", json);
}

// ─── Additional client → server events ────────────────────────────────────────

void FreeDVReporter::updateMessage(const std::string& message)
{
    localMessage_ = message;
    if (!fullyConnected_.load()) return;

    const std::string json = "{\"message\":\"" + jEscape(message) + "\"}";
    sio_.emit("message_update", json);
}

void FreeDVReporter::requestQsy(const std::string& destSid,
                                uint64_t           frequency,
                                const std::string& message)
{
    if (!fullyConnected_.load()) return;

    const std::string json =
        "{\"dest_sid\":\""  + jEscape(destSid)           + "\","
        "\"frequency\":"    + std::to_string(frequency)   + ","
        "\"message\":\""    + jEscape(message)            + "\"}";
    sio_.emit("qsy_request", json);
}

// ─── Connection lifecycle ─────────────────────────────────────────────────────

void FreeDVReporter::connect()
{
    fprintf(stderr, "reporter::connect()\n");
    // Build the Socket.IO auth JSON object according to the role.
    std::string auth;

    if (callsign_.empty() || gridSquare_.empty()) {
        auth = "{\"role\":\"view\",\"protocol_version\":2}";
    } else {
        const char* role = writeOnly_ ? "report_wo" : "report";
        auth = std::string("{")
             + "\"role\":\""         + role                   + "\","
             + "\"callsign\":\""     + jEscape(callsign_)     + "\","
             + "\"grid_square\":\""  + jEscape(gridSquare_)   + "\","
             + "\"version\":\""      + jEscape(version_)      + "\","
             + "\"rx_only\":"        + (rxOnly_ ? "true" : "false") + ","
             + "\"os\":\""           + kOs                    + "\","
             + "\"protocol_version\":2"
             + "}";
    }
    
    sio_.connect(host_, port_, auth);
}

void FreeDVReporter::disconnect()
{
    sio_.disconnect();
    fullyConnected_.store(false);
}

bool FreeDVReporter::isConnected() const
{
    return sio_.isConnected();
}

// ─── Station list ─────────────────────────────────────────────────────────────

std::vector<StationInfo> FreeDVReporter::getStations() const
{
    std::lock_guard<std::mutex> lock(stationMutex_);
    std::vector<StationInfo> result;
    result.reserve(stations_.size());
    for (const auto& kv : stations_)
        result.push_back(kv.second);
    return result;
}

StationInfo FreeDVReporter::getStation(const std::string& sid) const
{
    std::lock_guard<std::mutex> lock(stationMutex_);
    const auto it = stations_.find(sid);
    return (it != stations_.end()) ? it->second : StationInfo{};
}

void FreeDVReporter::setStationUpdateCallback(std::function<void()> cb)
{
    stationUpdateCb_ = std::move(cb);
}

void FreeDVReporter::setStationRemoveCallback(std::function<void(const std::string& sid)> cb)
{
    stationRemoveCb_ = std::move(cb);
}

void FreeDVReporter::setQsyRequestCallback(
    std::function<void(const std::string&, uint64_t, const std::string&)> cb)
{
    qsyRequestCb_ = std::move(cb);
}

// ─── Internal helpers ─────────────────────────────────────────────────────────

void FreeDVReporter::sendInitialState()
{
    if (inAnalog_.load()) return;

    if (localFreq_ != 0)
        freqChange(localFreq_);

    transmit(localMode_, localTransmitting_);
    updateMessage(localMessage_);
}

void FreeDVReporter::dispatchEvent(const std::string& name,
                                   const std::string& data)
{
    if      (name == "connection_successful") onConnectionSuccessful(data);
    else if (name == "new_connection")        onNewConnection(data);
    else if (name == "remove_connection")     onRemoveConnection(data);
    else if (name == "freq_change")           onFreqChange(data);
    else if (name == "tx_report")             onTxReport(data);
    else if (name == "rx_report")             onRxReport(data);
    else if (name == "message_update")        onMessageUpdate(data);
    else if (name == "qsy_request")           onQsyRequest(data);
}

// ─── Server → client event handlers ──────────────────────────────────────────

void FreeDVReporter::onConnectionSuccessful(const std::string& /*data*/)
{
    fullyConnected_.store(true);

    if (inAnalog_.load()) {
        sio_.emit("hide_self");
    } else {
        sendInitialState();
    }
}

void FreeDVReporter::onNewConnection(const std::string& data)
{
    std::cerr << "onNewConnection" << '\n';

    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    StationInfo info;
    info.sid          = jStr(root, "sid");
    info.callsign     = jStr(root, "callsign");
    info.grid_square  = jStr(root, "grid_square");
    info.version      = jStr(root, "version");
    info.rx_only      = jBool(root, "rx_only");
    info.connect_time = jStr(root, "connect_time");
    info.last_update  = jStr(root, "last_update");

    yyjson_doc_free(doc);

    if (info.sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        stations_[info.sid] = std::move(info);
        std::cerr << "sid = " << info.sid << '\n';
    }

    if (!suppressUpdateCb_ && stationUpdateCb_) stationUpdateCb_();
}

void FreeDVReporter::onRemoveConnection(const std::string& data)
{
    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);
    const std::string sid = jStr(root, "sid");
    yyjson_doc_free(doc);

    if (sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        stations_.erase(sid);
    }

    if (!suppressUpdateCb_) {
        if (stationRemoveCb_) stationRemoveCb_(sid);
        if (stationUpdateCb_) stationUpdateCb_();
    }
}

void FreeDVReporter::onFreqChange(const std::string& data)
{
    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    const std::string sid      = jStr(root, "sid");
    const uint64_t    freq     = jUint(root, "freq");
    const std::string upd      = jStr(root, "last_update");
    // Identity fields present in some server implementations
    const std::string callsign = jStr(root, "callsign");
    const std::string grid     = jStr(root, "grid_square");

    yyjson_doc_free(doc);

    if (sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        auto it = stations_.find(sid);
        if (it == stations_.end()) {
            StationInfo info;
            info.sid         = sid;
            info.callsign    = callsign;
            info.grid_square = grid;
            stations_[sid]   = std::move(info);
            it               = stations_.find(sid);
        }
        it->second.frequency      = freq;
        it->second.last_update    = upd;
        // A frequency change implicitly clears the last RX fields.
        it->second.rx_callsign    = "";
        it->second.rx_mode        = "";
        it->second.rx_snr         = 0.0;
        it->second.rx_last_update = "";
    }

    if (!suppressUpdateCb_ && stationUpdateCb_) stationUpdateCb_();
}

void FreeDVReporter::onTxReport(const std::string& data)
{
    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    const std::string sid      = jStr(root, "sid");
    const std::string mode     = jStr(root, "mode");
    const bool        txing    = jBool(root, "transmitting");
    const std::string lastTx   = jStr(root, "last_tx");
    const std::string upd      = jStr(root, "last_update");
    // Identity fields present in some server implementations
    const std::string callsign = jStr(root, "callsign");
    const std::string grid     = jStr(root, "grid_square");

    yyjson_doc_free(doc);

    if (sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        auto it = stations_.find(sid);
        if (it == stations_.end()) {
            StationInfo info;
            info.sid         = sid;
            info.callsign    = callsign;
            info.grid_square = grid;
            stations_[sid]   = std::move(info);
            it               = stations_.find(sid);
        }
        it->second.mode         = mode;
        it->second.transmitting = txing;
        it->second.last_tx      = lastTx;
        it->second.last_update  = upd;
    }

    if (!suppressUpdateCb_ && stationUpdateCb_) stationUpdateCb_();
}

void FreeDVReporter::onRxReport(const std::string& data)
{
    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    const std::string sid     = jStr(root, "sid");
    const std::string rxCall  = jStr(root, "callsign");
    const std::string rxMode  = jStr(root, "mode");
    const double      rxSnr   = jReal(root, "snr");
    const std::string upd     = jStr(root, "last_update");
    // receiver_callsign / receiver_grid_square identify the station doing
    // the receiving (its own callsign / grid, not the decoded signal).
    const std::string rxrCall = jStr(root, "receiver_callsign");
    const std::string rxrGrid = jStr(root, "receiver_grid_square");

    yyjson_doc_free(doc);

    if (sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        auto it = stations_.find(sid);
        if (it == stations_.end()) {
            StationInfo info;
            info.sid         = sid;
            info.callsign    = rxrCall;
            info.grid_square = rxrGrid;
            stations_[sid]   = std::move(info);
            it               = stations_.find(sid);
        }
        // Fill in identity if it was missing from new_connection.
        if (it->second.callsign.empty() && !rxrCall.empty())
            it->second.callsign = rxrCall;
        if (it->second.grid_square.empty() && !rxrGrid.empty())
            it->second.grid_square = rxrGrid;
        it->second.rx_callsign    = rxCall;
        it->second.rx_mode        = rxMode;
        it->second.rx_snr         = rxSnr;
        it->second.rx_last_update = upd;
        it->second.last_update    = upd;
    }

    if (!suppressUpdateCb_ && stationUpdateCb_) stationUpdateCb_();
}

void FreeDVReporter::onMessageUpdate(const std::string& data)
{
    if (data.empty()) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    const std::string sid = jStr(root, "sid");
    const std::string msg = jStr(root, "message");
    const std::string upd = jStr(root, "last_update");

    yyjson_doc_free(doc);

    if (sid.empty()) return;

    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        auto it = stations_.find(sid);
        if (it != stations_.end()) {
            it->second.message             = msg;
            it->second.message_last_update = upd;
            it->second.last_update         = upd;
        }
    }

    if (!suppressUpdateCb_ && stationUpdateCb_) stationUpdateCb_();
}

void FreeDVReporter::onQsyRequest(const std::string& data)
{
    if (data.empty() || !qsyRequestCb_) return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* root = yyjson_doc_get_root(doc);

    const std::string callsign  = jStr(root, "callsign");
    const uint64_t    frequency = jUint(root, "frequency");
    const std::string message   = jStr(root, "message");

    yyjson_doc_free(doc);

    qsyRequestCb_(callsign, frequency, message);
}

void FreeDVReporter::onBulkUpdate(const std::string& data)
{
    if (data.empty() || data[0] != '[') return;

    yyjson_doc* doc = yyjson_read(data.c_str(), data.size(), 0);
    if (!doc) return;
    yyjson_val* outerArr = yyjson_doc_get_root(doc);
    if (!yyjson_is_arr(outerArr)) {
        yyjson_doc_free(doc);
        return;
    }

    // Clear stale state before replaying the bulk snapshot.
    {
        std::lock_guard<std::mutex> lock(stationMutex_);
        stations_.clear();
    }

    // Suppress per-event callbacks while replaying; issue one at the end.
    suppressUpdateCb_ = true;

    size_t idx, max;
    yyjson_val* item;
    yyjson_arr_foreach(outerArr, idx, max, item) {
        // Each item is a two-element array: ["event_name", data_object]
        if (!yyjson_is_arr(item) || yyjson_arr_size(item) < 2) continue;

        yyjson_val* nameVal = yyjson_arr_get(item, 0);
        yyjson_val* dataVal = yyjson_arr_get(item, 1);
        if (!yyjson_is_str(nameVal)) continue;

        const std::string name = yyjson_get_str(nameVal);

        // Re-serialise the data value so dispatchEvent receives a JSON string,
        // exactly as if the event had arrived individually.
        size_t itemDataLen = 0;
        char*  itemDataStr = yyjson_val_write(dataVal, 0, &itemDataLen);
        const std::string itemData =
            itemDataStr ? std::string(itemDataStr, itemDataLen) : "{}";
        free(itemDataStr);

        dispatchEvent(name, itemData);
    }

    suppressUpdateCb_ = false;

    yyjson_doc_free(doc);

    if (stationUpdateCb_) stationUpdateCb_();
}
