#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "socket_io.h"

// ─── IReporter ────────────────────────────────────────────────────────────────

/// Abstract interface for activity-reporting backends (FreeDVReporter,
/// PskReporter, etc.).
class IReporter {
public:
    virtual ~IReporter() = default;

    /// Notify the reporter that the local operating frequency changed.
    virtual void freqChange(uint64_t frequency) = 0;

    /// Notify the reporter of a PTT state change.
    /// @param mode  FreeDV mode name (e.g. "FreeDV 700D").
    /// @param tx    true = now transmitting, false = now receiving.
    virtual void transmit(std::string mode, bool tx) = 0;

    /// Notify the reporter when the radio enters or leaves analogue mode.
    ///  - inAnalog == true  → send hide_self
    ///  - inAnalog == false → send show_self then re-send local state
    virtual void inAnalogMode(bool inAnalog) = 0;

    /// Log a successfully decoded signal.
    /// @param callsign  Decoded callsign.
    /// @param mode      FreeDV mode used.
    /// @param frequency Local RX frequency (Hz) — stored for context.
    /// @param snr       Signal-to-noise ratio in dB.
    virtual void addReceiveRecord(std::string callsign, std::string mode,
                                  uint64_t frequency, signed char snr) = 0;

    /// Flush any batched data. No-op for FreeDVReporter (events are sent
    /// immediately); used by PskReporter to flush its UDP packet.
    virtual void send() = 0;
};

// ─── StationInfo ──────────────────────────────────────────────────────────────

/// Accumulated real-time state for one remote station, populated by
/// server-to-client events delivered via the Socket.IO connection.
struct StationInfo {
    // ── Identity (new_connection) ──────────────────────────────────────────
    std::string sid;
    std::string callsign;
    std::string grid_square;
    std::string version;
    bool        rx_only      = false;
    std::string connect_time;   ///< ISO 8601 session-start timestamp
    std::string last_update;    ///< ISO 8601 timestamp of most recent event

    // ── Frequency (freq_change) ────────────────────────────────────────────
    uint64_t frequency = 0;     ///< Operating frequency in Hz

    // ── TX state (tx_report) ──────────────────────────────────────────────
    std::string mode;           ///< Current FreeDV mode name
    bool        transmitting = false;
    std::string last_tx;        ///< ISO 8601 timestamp of last transmission

    // ── Last decoded signal (rx_report) ───────────────────────────────────
    std::string rx_callsign;
    std::string rx_mode;
    double      rx_snr         = 0.0;
    std::string rx_last_update; ///< ISO 8601 timestamp of last RX report

    // ── Status message (message_update) ───────────────────────────────────
    std::string message;
    std::string message_last_update;
};

// ─── FreeDVReporter ───────────────────────────────────────────────────────────

/// Socket.IO client that reports local FreeDV activity to qso.freedv.org and
/// receives real-time activity from all other connected stations.
///
/// ### Thread safety
/// All public methods are safe to call from any thread.
/// Callbacks (setStationUpdateCallback, setQsyRequestCallback) are invoked
/// from the Socket.IO background thread.  GUI callers should marshal to the
/// UI thread (e.g. via `g_idle_add()` for GTK).
///
/// ### Roles
/// | Condition                          | Role        |
/// |------------------------------------|-------------|
/// | callsign or gridSquare is empty    | "view"      |
/// | Both set, writeOnly == false       | "report"    |
/// | Both set, writeOnly == true        | "report_wo" |
class FreeDVReporter : public IReporter {
public:
    /// @param callsign   Local amateur callsign (empty → view-only).
    /// @param gridSquare Maidenhead locator (only first 6 chars are used).
    /// @param version    Software version string sent in the auth payload.
    /// @param rxOnly     True if the station cannot transmit.
    /// @param writeOnly  True to report activity but not receive display data.
    /// @param host       Server hostname.
    /// @param port       Server TCP port.
    explicit FreeDVReporter(const std::string& callsign   = "",
                            const std::string& gridSquare = "",
                            const std::string& version    = "RADAE v1.0",
                            bool               rxOnly     = false,
                            bool               writeOnly  = true, // TODO change to receive station updates
                            const std::string& host       = "qso.freedv.org",
                            int                port       = 80);
    ~FreeDVReporter() override;

    // Non-copyable (owns a background thread via SocketIO).
    FreeDVReporter(const FreeDVReporter&)            = delete;
    FreeDVReporter& operator=(const FreeDVReporter&) = delete;

    // ── IReporter ──────────────────────────────────────────────────────────
    void freqChange(uint64_t frequency) override;
    void transmit(std::string mode, bool tx) override;
    void inAnalogMode(bool inAnalog) override;
    void addReceiveRecord(std::string callsign, std::string mode,
                          uint64_t frequency, signed char snr) override;
    void send() override {}  // no-op: Socket.IO events are sent immediately

    // ── Additional client → server events ──────────────────────────────────

    /// Broadcast a free-text status message (empty string clears it).
    void updateMessage(const std::string& message);

    /// Ask another station to QSY to @p frequency.
    /// @param destSid  Session ID of the target station.
    /// @param frequency Frequency in Hz to request.
    /// @param message  Optional free-text message.
    void requestQsy(const std::string& destSid,
                    uint64_t           frequency,
                    const std::string& message = "");

    // ── Connection lifecycle ────────────────────────────────────────────────
    void connect();
    void disconnect();
    bool isConnected() const;

    // ── Station list ────────────────────────────────────────────────────────

    /// Return a snapshot of all currently tracked stations (thread-safe).
    std::vector<StationInfo> getStations() const;

    /// Return the station with the given SID, or a default StationInfo
    /// (empty sid field) if not found.
    StationInfo getStation(const std::string& sid) const;

    // ── Callbacks ───────────────────────────────────────────────────────────

    /// Invoked (from the Socket.IO thread) whenever the station list changes.
    void setStationUpdateCallback(std::function<void()> cb);

    /// Invoked (from the Socket.IO thread) when a station disconnects.
    /// The argument is the SID of the removed station.
    void setStationRemoveCallback(std::function<void(const std::string& sid)> cb);

    /// Invoked when a remote station sends a qsy_request targeting this client.
    void setQsyRequestCallback(
        std::function<void(const std::string& callsign,
                           uint64_t           frequency,
                           const std::string& message)> cb);

private:
    // ── Server → client event handlers ────────────────────────────────────
    void onConnectionSuccessful(const std::string& data);
    void onNewConnection       (const std::string& data);
    void onRemoveConnection    (const std::string& data);
    void onFreqChange          (const std::string& data);
    void onTxReport            (const std::string& data);
    void onRxReport            (const std::string& data);
    void onMessageUpdate       (const std::string& data);
    void onQsyRequest          (const std::string& data);
    void onBulkUpdate          (const std::string& data);

    /// Route a single named event to the appropriate handler.
    /// Used both for direct events and for replayed bulk_update entries.
    void dispatchEvent(const std::string& name, const std::string& data);

    /// Emit the local station's current frequency, TX state, and message
    /// after the connection is established or after leaving analogue mode.
    void sendInitialState();

    // ── Configuration ──────────────────────────────────────────────────────
    std::string callsign_;
    std::string gridSquare_;  ///< Already truncated to 6 chars
    std::string version_;
    bool        rxOnly_;
    bool        writeOnly_;
    std::string host_;
    int         port_;

    // ── Local state (last values sent to the server) ──────────────────────
    uint64_t    localFreq_         = 0;
    std::string localMode_;
    bool        localTransmitting_ = false;
    std::atomic<bool> inAnalog_{false};
    std::string localMessage_;

    // Written from the Socket.IO thread, read from the caller thread.
    std::atomic<bool> fullyConnected_{false};

    // Only accessed from the Socket.IO callback thread — no atomic needed.
    // When true, individual event handlers skip their stationUpdateCb_ call
    // so that bulk_update can issue a single notification at the end.
    bool suppressUpdateCb_ = false;

    // ── Socket.IO transport ────────────────────────────────────────────────
    SocketIO sio_;

    // ── Station store ──────────────────────────────────────────────────────
    mutable std::mutex               stationMutex_;
    std::map<std::string, StationInfo> stations_;

    // ── User callbacks ─────────────────────────────────────────────────────
    std::function<void()> stationUpdateCb_;
    std::function<void(const std::string&)> stationRemoveCb_;
    std::function<void(const std::string&, uint64_t, const std::string&)>
        qsyRequestCb_;
};
