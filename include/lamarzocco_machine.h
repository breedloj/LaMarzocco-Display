#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "lamarzocco_client.h"
#include "lamarzocco_websocket.h"

class LaMarzoccoMachine {
public:
    enum class QueueResult : uint8_t {
        Queued,
        AlreadyPending,
        Unavailable
    };

    LaMarzoccoMachine(LaMarzoccoClient& client, LaMarzoccoWebSocket& websocket);
    ~LaMarzoccoMachine();
    
    // Set power on/off
    bool set_power(bool enabled);
    
    // Get current power state (from last command or websocket update)
    bool get_power_state() const;
    
    // Toggle power
    bool toggle_power();

    // Queue a power toggle for the network loop. This is non-blocking and is
    // the API UI callbacks should use.
    QueueResult queue_power_toggle();
    
    // Set steam boiler on/off
    bool set_steam(bool enabled);
    
    // Get current steam boiler state
    bool get_steam_state() const;
    
    // Toggle steam boiler
    bool toggle_steam();

    // Queue a steam toggle for the network loop. This is non-blocking and is
    // the API UI callbacks should use.
    QueueResult queue_steam_toggle();
    
    // Connect websocket and start listening
    bool connect_websocket();
    
    // Check if websocket is connected
    bool is_websocket_connected() const;
    
    // Disconnect websocket
    void disconnect_websocket();
    
    // Loop (call in main loop)
    void loop();

    // Request a refresh of coffee/flush counters
    void request_stats_refresh();
    
private:
    enum class CommandType : uint8_t {
        Power,
        Steam
    };

    struct QueuedCommand {
        CommandType type;
        bool enabled;
    };

    static constexpr uint8_t POWER_PENDING_BIT = 1U << 0;
    static constexpr uint8_t STEAM_PENDING_BIT = 1U << 1;

    LaMarzoccoClient& _client;
    LaMarzoccoWebSocket& _websocket;
    bool _power_state;
    bool _steam_state;
    bool _power_state_valid;
    bool _steam_state_valid;
    QueueHandle_t _command_queue;
    uint8_t _pending_commands;
    mutable portMUX_TYPE _state_mux;
    bool _stats_refresh_pending = false;
    unsigned long _last_stats_refresh_ms = 0;
    String _last_machine_status;
    bool _machine_status_seen = false;
    bool _last_brewing_state = false;
    bool _last_brewing_state_valid = false;
    int64_t _last_coffee_event_ms = 0;
    int64_t _last_flush_event_ms = 0;
    unsigned long _last_websocket_dashboard_ms = 0;
    unsigned long _last_dashboard_poll_ms = 0;
    bool _dashboard_refresh_pending = false;
    unsigned long _dashboard_refresh_due_ms = 0;
    uint8_t _dashboard_refresh_attempts = 0;
    unsigned long _last_reconnect_attempt_ms = 0;
    unsigned long _reconnect_interval_ms = 10000;
    bool _connection_attempt_pending = false;
    uint8_t _consecutive_cloud_failures = 0;
    
    // WebSocket message handler
    static void _websocket_message_handler(const String& message);
    static LaMarzoccoMachine* _instance;

    bool _handle_dashboard_message(const String& message);
    QueueResult _queue_toggle(CommandType type);
    bool _process_next_command();
    void _set_power_state(bool enabled);
    void _set_steam_state(bool enabled);
    void _note_cloud_failure();
    void _note_cloud_success();
    bool _refresh_shot_counters();
    bool _refresh_dashboard();
};
