#pragma once

#include <Arduino.h>
#include <WebSocketsClient.h>
#include "lamarzocco_client.h"

class LaMarzoccoWebSocket {
public:
    LaMarzoccoWebSocket(LaMarzoccoClient& client);
    ~LaMarzoccoWebSocket();
    
    // Connect to websocket
    bool connect(const String& serial_number);
    
    // Disconnect
    void disconnect();
    
    // Check if connected
    bool is_connected() const { return _connected; }

    // A reconnect must go through connect() so authentication and signed
    // upgrade headers are regenerated instead of reusing stale values.
    bool needs_reconnect() const { return _reconnect_required; }

    // True while the WebSocket/STOMP handshake is still in progress.
    bool is_connecting() const { return _connecting; }
    
    // Loop (call in main loop or task)
    void loop();
    
    // Set callback for incoming messages
    void set_message_callback(void (*callback)(const String& message));
    
private:
    LaMarzoccoClient& _client;
    WebSocketsClient _ws;
    bool _connected;
    bool _connecting;
    bool _transport_connected;
    bool _reconnect_required;
    bool _transport_release_pending;
    bool _manual_disconnect;
    String _serial_number;
    String _subscription_id;
    String _cached_token;  // Cache token before connecting to avoid accessing client in callback
    String _fragment_buffer;
    bool _receiving_text_fragment;
    unsigned long _connect_started_ms;
    unsigned long _last_stomp_activity_ms;
    unsigned long _last_stomp_heartbeat_ms;
    unsigned long _stomp_outgoing_interval_ms;
    unsigned long _stomp_incoming_interval_ms;
    void (*_message_callback)(const String& message);
    
    // STOMP protocol helpers
    String _encode_stomp_message(const String& command, const String& headers, const String& body = "");
    bool _decode_stomp_message(const String& message, String& command, String& headers, String& body);
    String _get_stomp_header(const String& headers, const String& name) const;
    void _configure_stomp_heartbeats(const String& headers);
    bool _append_text_fragment(uint8_t* payload, size_t length);
    void _handle_stomp_text(const String& message);
    void _release_transport();
    void _mark_reconnect_required(const char* reason);
    void _handle_websocket_event(WStype_t type, uint8_t* payload, size_t length);
    static void _ws_event_handler(WStype_t type, uint8_t* payload, size_t length);
    static LaMarzoccoWebSocket* _instance;
};
