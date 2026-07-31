#include "lamarzocco_websocket.h"
#include "config.h"
#include <esp_random.h>

static const char* WS_BASE_URL = "lion.lamarzocco.io";
static const unsigned long CLIENT_HEARTBEAT_SEND_MS = 10000;
static const unsigned long CLIENT_HEARTBEAT_RECEIVE_MS = 10000;
static const unsigned long STOMP_CONNECT_TIMEOUT_MS = 20000;
static const unsigned long STOMP_STALE_GRACE_MS = 5000;
static const unsigned long LIBRARY_RECONNECT_DISABLED_MS = 0xFFFFFFFFUL;
static const size_t MAX_STOMP_FRAME_BYTES = 32768;

LaMarzoccoWebSocket* LaMarzoccoWebSocket::_instance = nullptr;

LaMarzoccoWebSocket::LaMarzoccoWebSocket(LaMarzoccoClient& client)
    : _client(client),
      _connected(false),
      _connecting(false),
      _transport_connected(false),
      _reconnect_required(false),
      _transport_release_pending(false),
      _manual_disconnect(false),
      _receiving_text_fragment(false),
      _connect_started_ms(0),
      _last_stomp_activity_ms(0),
      _last_stomp_heartbeat_ms(0),
      _stomp_outgoing_interval_ms(0),
      _stomp_incoming_interval_ms(0),
      _message_callback(nullptr) {
    _instance = this;
    _ws.onEvent(_ws_event_handler);

    // Automatic reconnect in WebSocketsClient reuses the token and signed
    // upgrade headers from the old attempt. Reconnects are intentionally
    // initiated through connect(), which refreshes both.
    _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);

    // RFC 6455 ping/pong is a fallback when the STOMP server does not negotiate
    // application-level heartbeats.
    _ws.enableHeartbeat(15000, 5000, 2);
}

LaMarzoccoWebSocket::~LaMarzoccoWebSocket() {
    if (_instance == this) {
        _instance = nullptr;
    }
    disconnect();
}

void LaMarzoccoWebSocket::_ws_event_handler(WStype_t type,
                                             uint8_t* payload,
                                             size_t length) {
    if (_instance != nullptr) {
        _instance->_handle_websocket_event(type, payload, length);
    }
}

String LaMarzoccoWebSocket::_encode_stomp_message(const String& command,
                                                  const String& headers,
                                                  const String& body) {
    String message = command + "\n";
    message += headers;
    if (!headers.endsWith("\n")) {
        message += "\n";
    }
    message += "\n";
    if (body.length() > 0) {
        message += body;
    }
    message += '\x00';
    return message;
}

bool LaMarzoccoWebSocket::_decode_stomp_message(const String& message,
                                                String& command,
                                                String& headers,
                                                String& body) {
    String normalized = message;
    normalized.replace("\r\n", "\n");

    // Heartbeats may precede a frame when frames are coalesced.
    size_t frame_start = 0;
    while (frame_start < normalized.length() &&
           normalized[frame_start] == '\n') {
        ++frame_start;
    }
    if (frame_start > 0) {
        normalized.remove(0, frame_start);
    }

    int header_end = normalized.indexOf("\n\n");
    if (header_end < 0) {
        return false;
    }

    int command_end = normalized.indexOf('\n');
    if (command_end < 0 || command_end > header_end) {
        return false;
    }

    command = normalized.substring(0, command_end);
    command.trim();
    headers = command_end + 1 < header_end
                  ? normalized.substring(command_end + 1, header_end)
                  : "";

    int body_start = header_end + 2;
    int body_end = normalized.indexOf('\x00', body_start);
    if (body_end < 0) {
        body_end = normalized.length();
    }
    body = body_end > body_start
               ? normalized.substring(body_start, body_end)
               : "";
    return command.length() > 0;
}

String LaMarzoccoWebSocket::_get_stomp_header(const String& headers,
                                              const String& name) const {
    int line_start = 0;
    while (line_start < static_cast<int>(headers.length())) {
        int line_end = headers.indexOf('\n', line_start);
        if (line_end < 0) {
            line_end = headers.length();
        }

        String line = headers.substring(line_start, line_end);
        int separator = line.indexOf(':');
        if (separator > 0) {
            String key = line.substring(0, separator);
            key.trim();
            if (key == name) {
                String value = line.substring(separator + 1);
                value.trim();
                return value;
            }
        }
        line_start = line_end + 1;
    }
    return "";
}

void LaMarzoccoWebSocket::_configure_stomp_heartbeats(
    const String& headers) {
    _stomp_outgoing_interval_ms = 0;
    _stomp_incoming_interval_ms = 0;

    String heartbeat = _get_stomp_header(headers, "heart-beat");
    int comma = heartbeat.indexOf(',');
    if (comma > 0) {
        unsigned long server_can_send =
            static_cast<unsigned long>(heartbeat.substring(0, comma).toInt());
        unsigned long server_wants_receive =
            static_cast<unsigned long>(heartbeat.substring(comma + 1).toInt());

        // STOMP 1.2: each effective interval is the maximum of the sender's
        // capability and the receiver's requested minimum.
        if (CLIENT_HEARTBEAT_SEND_MS > 0 && server_wants_receive > 0) {
            _stomp_outgoing_interval_ms =
                max(CLIENT_HEARTBEAT_SEND_MS, server_wants_receive);
        }
        if (CLIENT_HEARTBEAT_RECEIVE_MS > 0 && server_can_send > 0) {
            _stomp_incoming_interval_ms =
                max(CLIENT_HEARTBEAT_RECEIVE_MS, server_can_send);
        }
    }

    unsigned long now = millis();
    _last_stomp_activity_ms = now;
    _last_stomp_heartbeat_ms = now;

    debug("STOMP heartbeat negotiated (send/receive ms): ");
    debug(_stomp_outgoing_interval_ms);
    debug("/");
    debugln(_stomp_incoming_interval_ms);
}

bool LaMarzoccoWebSocket::_append_text_fragment(uint8_t* payload,
                                                size_t length) {
    if (payload == nullptr && length > 0) {
        return false;
    }

    if (length > MAX_STOMP_FRAME_BYTES ||
        _fragment_buffer.length() >
            MAX_STOMP_FRAME_BYTES - length) {
        return false;
    }

    if (length == 0) {
        return true;
    }

    return _fragment_buffer.concat(
        reinterpret_cast<const char*>(payload),
        static_cast<unsigned int>(length));
}

void LaMarzoccoWebSocket::_handle_stomp_text(const String& message) {
    _last_stomp_activity_ms = millis();

    // A STOMP heartbeat is an LF outside a frame.
    bool heartbeat_only = message.length() > 0;
    for (size_t i = 0; i < message.length(); ++i) {
        char c = message[i];
        if (c != '\n' && c != '\r' && c != '\0') {
            heartbeat_only = false;
            break;
        }
    }
    if (heartbeat_only) {
        return;
    }

    String command, headers, body;
    if (!_decode_stomp_message(message, command, headers, body)) {
        debugln("Ignoring malformed STOMP frame");
        return;
    }

    if (command == "CONNECTED") {
        _configure_stomp_heartbeats(headers);

        _subscription_id = LaMarzoccoAuth::generate_uuid();
        if (_subscription_id.length() == 0) {
            _mark_reconnect_required(
                "failed to generate STOMP subscription ID");
            return;
        }

        String subscribe_headers;
        subscribe_headers.reserve(256);
        subscribe_headers = "destination:/ws/sn/";
        subscribe_headers += _serial_number;
        subscribe_headers += "/dashboard\nack:auto\nid:";
        subscribe_headers += _subscription_id;
        subscribe_headers += "\ncontent-length:0\n";

        String subscribe_message =
            _encode_stomp_message("SUBSCRIBE", subscribe_headers);
        if (!_ws.sendTXT(subscribe_message)) {
            _mark_reconnect_required("failed to send STOMP SUBSCRIBE");
            return;
        }

        _connected = true;
        _connecting = false;
        _reconnect_required = false;
        debugln("WebSocket connected and subscribed");
    } else if (command == "MESSAGE") {
        if (_message_callback != nullptr) {
            _message_callback(body);
        }
    } else if (command == "ERROR") {
        debug("STOMP ERROR: ");
        debugln(body);
        // Broker-side authentication failures can precede the locally tracked
        // token expiry. Ensure the next reconnect obtains a fresh bearer
        // instead of looping with the rejected cached token.
        _client.invalidate_access_token();
        _mark_reconnect_required("server returned STOMP ERROR");
    } else if (command != "RECEIPT") {
        debug("Ignoring STOMP command: ");
        debugln(command);
    }
}

void LaMarzoccoWebSocket::_release_transport() {
    _transport_release_pending = false;
    _manual_disconnect = true;
    // WebSocketsClient::disconnect() also runs its cleanup path for a
    // non-null failed TCP/TLS client, not only for a completed WebSocket.
    _ws.disconnect();
    _manual_disconnect = false;
}

void LaMarzoccoWebSocket::_mark_reconnect_required(const char* reason) {
    if (reason != nullptr && reason[0] != '\0') {
        debug("WebSocket reconnect required: ");
        debugln(reason);
    }

    _connected = false;
    _connecting = false;
    _transport_connected = false;
    _reconnect_required = true;
    _subscription_id = "";
    _fragment_buffer = "";
    _receiving_text_fragment = false;
    _stomp_outgoing_interval_ms = 0;
    _stomp_incoming_interval_ms = 0;
    _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);
    // This method can run from inside WebSocketsClient's callback. Defer
    // deletion of its network object until control has returned to loop().
    _transport_release_pending = true;
}

void LaMarzoccoWebSocket::_handle_websocket_event(WStype_t type,
                                                  uint8_t* payload,
                                                  size_t length) {
    if (_instance != this) {
        return;
    }

    switch (type) {
        case WStype_DISCONNECTED:
            debugln("WebSocket disconnected");
            _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);
            _connected = false;
            _connecting = false;
            _transport_connected = false;
            _transport_release_pending = false;
            _subscription_id = "";
            _fragment_buffer = "";
            _receiving_text_fragment = false;
            _stomp_outgoing_interval_ms = 0;
            _stomp_incoming_interval_ms = 0;
            if (!_manual_disconnect) {
                _reconnect_required = true;
            }
            break;

        case WStype_CONNECTED: {
            _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);
            _transport_connected = true;
            _connecting = true;
            debugln("WebSocket transport connected; sending STOMP CONNECT");

            if (_cached_token.length() == 0) {
                _mark_reconnect_required("cached access token is empty");
                break;
            }

            String connect_headers = "host:" + String(WS_BASE_URL) + "\n";
            connect_headers += "accept-version:1.2,1.1,1.0\n";
            connect_headers += "heart-beat:";
            connect_headers += String(CLIENT_HEARTBEAT_SEND_MS);
            connect_headers += ",";
            connect_headers += String(CLIENT_HEARTBEAT_RECEIVE_MS);
            connect_headers += "\n";
            connect_headers += "Authorization:Bearer " + _cached_token + "\n";

            String connect_message =
                _encode_stomp_message("CONNECT", connect_headers);
            if (!_ws.sendTXT(connect_message)) {
                _mark_reconnect_required("failed to send STOMP CONNECT");
            }
            break;
        }

        case WStype_TEXT: {
            String message = String(reinterpret_cast<char*>(payload), length);
            _handle_stomp_text(message);
            break;
        }

        case WStype_FRAGMENT_TEXT_START:
            _fragment_buffer = "";
            _receiving_text_fragment = true;
            if (!_append_text_fragment(payload, length)) {
                _mark_reconnect_required(
                    "fragmented STOMP frame is too large");
            }
            break;

        case WStype_FRAGMENT:
            if (_receiving_text_fragment &&
                !_append_text_fragment(payload, length)) {
                _mark_reconnect_required(
                    "fragmented STOMP frame is too large");
            }
            break;

        case WStype_FRAGMENT_FIN:
            if (_receiving_text_fragment) {
                if (!_append_text_fragment(payload, length)) {
                    _mark_reconnect_required(
                        "fragmented STOMP frame is too large");
                    break;
                }
                _receiving_text_fragment = false;
                String complete_message = _fragment_buffer;
                _fragment_buffer = "";
                _handle_stomp_text(complete_message);
            }
            break;

        case WStype_FRAGMENT_BIN_START:
            _fragment_buffer = "";
            _receiving_text_fragment = false;
            break;

        case WStype_ERROR:
            if (payload != nullptr && length > 0) {
                debug("WebSocket error: ");
                debugln(String(reinterpret_cast<char*>(payload), length));
            } else {
                debugln("WebSocket error");
            }
            _mark_reconnect_required("transport error");
            break;

        case WStype_PONG:
        case WStype_PING:
        case WStype_BIN:
            break;

        default:
            break;
    }
}

bool LaMarzoccoWebSocket::connect(const String& serial_number) {
    if (_connected) {
        return true;
    }

    unsigned long now = millis();
    if (_connecting &&
        _serial_number == serial_number &&
        now - _connect_started_ms < STOMP_CONNECT_TIMEOUT_MS) {
        // A second caller should not tear down a healthy in-flight handshake.
        return true;
    }

    if (serial_number.length() == 0) {
        debugln("Cannot connect WebSocket without a serial number");
        _reconnect_required = true;
        return false;
    }

    // Stop any timed-out/partial attempt before replacing its header storage.
    if (_transport_release_pending || _connecting ||
        _transport_connected || _ws.isConnected()) {
        _release_transport();
    }

    _serial_number = serial_number;
    _connected = false;
    _connecting = false;
    _transport_connected = false;
    _reconnect_required = false;
    _subscription_id = "";
    _fragment_buffer = "";
    _receiving_text_fragment = false;

    // Fetching a token here ensures every explicit reconnect can refresh an
    // expired token before building the STOMP CONNECT frame.
    if (!_client.get_access_token()) {
        debugln("Failed to get access token for WebSocket");
        _reconnect_required = true;
        return false;
    }

    _cached_token = _client.get_access_token_string();
    if (_cached_token.length() == 0) {
        debugln("WebSocket access token is empty");
        _reconnect_required = true;
        return false;
    }

    InstallationKey key;
    if (!_client.get_installation_key(key)) {
        debugln("Failed to get installation key for WebSocket");
        _reconnect_required = true;
        return false;
    }

    String installation_id, timestamp, nonce, signature;
    LaMarzoccoAuth::generate_extra_request_headers(
        key,
        installation_id,
        timestamp,
        nonce,
        signature);
    if (signature.length() == 0) {
        debugln("Failed to sign WebSocket upgrade request");
        _reconnect_required = true;
        return false;
    }

    // WebSocketsClient copies extraHeaders into its own String storage.
    String upgrade_headers;
    upgrade_headers.reserve(400);
    upgrade_headers = "X-App-Installation-Id: ";
    upgrade_headers += installation_id;
    upgrade_headers += "\r\nX-Timestamp: ";
    upgrade_headers += timestamp;
    upgrade_headers += "\r\nX-Nonce: ";
    upgrade_headers += nonce;
    upgrade_headers += "\r\nX-Request-Signature: ";
    upgrade_headers += signature;
    _ws.setExtraHeaders(upgrade_headers.c_str());

    yield();
    if (ESP.getFreeHeap() < 20000) {
        debugln("Low memory; WebSocket connection deferred");
        _reconnect_required = true;
        return false;
    }

    _connect_started_ms = millis();
    _last_stomp_activity_ms = _connect_started_ms;
    _last_stomp_heartbeat_ms = _connect_started_ms;
    _connecting = true;
    // Permit exactly the first transport attempt. loop() restores the very
    // long interval immediately afterward so the library cannot retry with
    // cached authentication headers.
    _ws.setReconnectInterval(0);
    _ws.beginSSL(WS_BASE_URL, 443, "/ws/connect");
    debugln("WebSocket connection initiated");
    return true;
}

void LaMarzoccoWebSocket::disconnect() {
    bool was_connected = _connected;
    _manual_disconnect = true;
    _connected = false;
    _connecting = false;
    _transport_connected = false;
    _reconnect_required = false;

    if (was_connected && _subscription_id.length() > 0 &&
        _ws.isConnected()) {
        String unsubscribe_headers = "id:" + _subscription_id + "\n";
        String unsubscribe_message =
            _encode_stomp_message("UNSUBSCRIBE", unsubscribe_headers);
        _ws.sendTXT(unsubscribe_message);
    }

    _release_transport();
    _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);
    _ws.setExtraHeaders(nullptr);
    _subscription_id = "";
    _cached_token = "";
    _fragment_buffer = "";
    _receiving_text_fragment = false;
    _stomp_outgoing_interval_ms = 0;
    _stomp_incoming_interval_ms = 0;
    _manual_disconnect = false;
}

void LaMarzoccoWebSocket::loop() {
    if (_instance != this) {
        return;
    }

    if (_transport_release_pending) {
        _release_transport();
    }

    _ws.loop();
    if (_transport_release_pending) {
        _release_transport();
    }
    _ws.setReconnectInterval(LIBRARY_RECONNECT_DISABLED_MS);
    unsigned long now = millis();

    if (_connecting &&
        now - _connect_started_ms >= STOMP_CONNECT_TIMEOUT_MS) {
        _mark_reconnect_required("STOMP handshake timed out");
        return;
    }

    if (!_connected) {
        return;
    }

    if (_stomp_outgoing_interval_ms > 0 &&
        now - _last_stomp_heartbeat_ms >=
            _stomp_outgoing_interval_ms) {
        String heartbeat = "\n";
        if (!_ws.sendTXT(heartbeat)) {
            _mark_reconnect_required("failed to send STOMP heartbeat");
            return;
        }
        _last_stomp_heartbeat_ms = now;
    }

    if (_stomp_incoming_interval_ms > 0) {
        uint64_t stale_after =
            static_cast<uint64_t>(_stomp_incoming_interval_ms) * 2ULL +
            STOMP_STALE_GRACE_MS;
        if (static_cast<uint64_t>(now - _last_stomp_activity_ms) >
            stale_after) {
            _mark_reconnect_required("STOMP connection became stale");
        }
    }
}

void LaMarzoccoWebSocket::set_message_callback(
    void (*callback)(const String& message)) {
    _message_callback = callback;
}
