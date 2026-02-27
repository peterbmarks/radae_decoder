#include "socket_io.h"

#include <ixwebsocket/IXWebSocket.h>

#include <iostream>

// ─── Engine.IO / Socket.IO frame parser ──────────────────────────────────────
//
// Socket.IO EVENT packets arrive as:
//   42["event_name"]          (no data)
//   42["event_name",<value>]  (with data)
//
// This helper extracts the event name and the raw JSON data value.
// It does not use a JSON parser so that socket_io.cpp has no yyjson dependency.
// The only assumption is that the event name is an unescaped ASCII string,
// which is always true for Socket.IO event names.

static bool sio_parse_event(const std::string& payload,
                             std::string&       outName,
                             std::string&       outData)
{
    // payload starts immediately after "42", so it should be:
    //   ["event_name", ...]
    if (payload.size() < 4 || payload[0] != '[' || payload[1] != '"')
        return false;

    // Find the closing quote of the event name.
    const size_t nameEnd = payload.find('"', 2);
    if (nameEnd == std::string::npos)
        return false;

    outName = payload.substr(2, nameEnd - 2);

    // Look for data after the event name: ,"<data>"]
    const size_t comma = payload.find(',', nameEnd + 1);
    if (comma == std::string::npos) {
        outData.clear();
        return true;
    }

    // Data runs from after the comma to just before the final ']'.
    // rfind(']') always locates the outer array's closing bracket because
    // even nested arrays end before the very last character of the packet.
    const size_t lastBracket = payload.rfind(']');
    if (lastBracket == std::string::npos || lastBracket <= comma) {
        outData.clear();
        return true;
    }

    outData = payload.substr(comma + 1, lastBracket - comma - 1);

    // Trim any leading whitespace (the spec allows optional whitespace).
    const size_t first = outData.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        outData.clear();
    else if (first > 0)
        outData = outData.substr(first);

    return true;
}

// ─── SocketIO ─────────────────────────────────────────────────────────────────

SocketIO::SocketIO()
    : ws_(std::make_unique<ix::WebSocket>())
{}

SocketIO::~SocketIO()
{
    disconnect();
}

void SocketIO::on(const std::string& event, EventCallback cb)
{
    std::lock_guard<std::mutex> lock(handlersMutex_);
    handlers_[event] = std::move(cb);
}

void SocketIO::onConnect(std::function<void()> cb)
{
    connectCb_ = std::move(cb);
}

void SocketIO::onDisconnect(std::function<void()> cb)
{
    disconnectCb_ = std::move(cb);
}

void SocketIO::connect(const std::string& host, int port,
                       const std::string& authJson)
{
    authJson_ = authJson;
    // example: URL: wss://qso.freedv.org/socket.io/?EIO=4&transport=websocket&sid=q_U8PuyOCv7fefPzDV3E

    // Build the Socket.IO WebSocket URL.
    // EIO=4 selects Engine.IO v4 (used by Socket.IO v3/v4).
    // transport=websocket skips HTTP long-polling and upgrades immediately.
    const std::string url = "ws://" + host + ":" + std::to_string(port)
                          + "/socket.io/?EIO=4&transport=websocket";
    ws_->setUrl(url);
    std::cerr << "calling reporter url = " << url << '\n';

    // IXWebSocket fires this callback from its own background thread.
    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Message:
            onRawMessage(msg->str);
            break;
        case ix::WebSocketMessageType::Open:
            // WebSocket handshake complete; wait for Engine.IO OPEN ('0').
            break;
        case ix::WebSocketMessageType::Close:
            sioConnected_.store(false);
            if (disconnectCb_) disconnectCb_();
            break;
        case ix::WebSocketMessageType::Error:
            sioConnected_.store(false);
            std::cerr << "[SocketIO] WebSocket error: "
                      << msg->errorInfo.reason << '\n';
            break;
        default:
            break;
        }
    });

    // start() is non-blocking; it spawns a background thread.
    // IXWebSocket enables automatic reconnection by default.
    ws_->start();
}

void SocketIO::disconnect()
{
    if (ws_) {
        ws_->stop();     // blocks until the background thread exits
        sioConnected_.store(false);
    }
}

void SocketIO::emit(const std::string& event, const std::string& dataJson)
{
    if (!sioConnected_.load()) return;

    // Socket.IO EVENT packet: 42["event_name"] or 42["event_name",<data>]
    std::string packet;
    packet.reserve(10 + event.size() + dataJson.size());
    packet += "42[\"";
    packet += event;
    packet += '"';
    if (!dataJson.empty()) {
        packet += ',';
        packet += dataJson;
    }
    packet += ']';
    fprintf(stderr, "%s\n", packet.c_str());
    sendRaw(packet);
}

// ─── Internal ─────────────────────────────────────────────────────────────────

void SocketIO::sendRaw(const std::string& packet)
{
    std::cerr << "sendRaw sending: " << packet << '\n';
    ws_->send(packet);
}

void SocketIO::onRawMessage(const std::string& msg)
{
    std::cerr << "socketIO::onRawMessage received: " << msg << '\n';
    if (msg.empty()) return;

    switch (msg[0]) {

    case '0':
        // Engine.IO OPEN – the server has sent its session info JSON.
        // Reply with a Socket.IO CONNECT packet (optionally carrying auth).
        {
            std::string connectPkt = "40";
            if (!authJson_.empty() && authJson_ != "{}") {
                connectPkt += "{\"auth\":";
                connectPkt += authJson_;
                connectPkt += '}';
            }
            std::cerr << "got connect packet, sending reply: " << connectPkt << '\n';
            sendRaw(connectPkt);
        }
        break;

    case '1':
        // Engine.IO CLOSE.
        sioConnected_.store(false);
        if (disconnectCb_) disconnectCb_();
        break;

    case '2':
        // Engine.IO PING – server-side heartbeat; reply with PONG.
        sendRaw("3");
        break;

    case '3':
        // Engine.IO PONG (response to a client-initiated ping, unused here).
        break;

    case '4': {
        // Engine.IO MESSAGE – contains a Socket.IO packet.
        if (msg.size() < 2) return;

        const char       sioType = msg[1];
        const std::string payload = msg.substr(2);

        switch (sioType) {

        case '0':
            // Socket.IO CONNECT ACK – we are now fully connected.
            std::cerr << "got Socket.IO connect ACK, we are fully connected." << '\n';
            sioConnected_.store(true);
            if (connectCb_) connectCb_();
            break;

        case '1':
            // Socket.IO DISCONNECT.
            std::cerr << "got Socket.IO disconnect" << '\n';
            sioConnected_.store(false);
            if (disconnectCb_) disconnectCb_();
            break;

        case '2': {
            // Socket.IO EVENT – parse and dispatch.
            std::cerr << "got Socket.IO EVENT" << '\n';
            std::string eventName, eventData;
            if (sio_parse_event(payload, eventName, eventData))
                dispatchEvent(eventName, eventData);
            break;
        }

        case '4':
            // Socket.IO CONNECT_ERROR.
            sioConnected_.store(false);
            std::cerr << "[SocketIO] CONNECT_ERROR: " << payload << '\n';
            if (disconnectCb_) disconnectCb_();
            break;

        default:
            break;
        }
        break;
    }

    default:
        break;
    }
}

void SocketIO::dispatchEvent(const std::string& name,
                             const std::string& dataJson)
{
    std::lock_guard<std::mutex> lock(handlersMutex_);
    auto it = handlers_.find(name);
    if (it != handlers_.end())
        it->second(dataJson);
}
