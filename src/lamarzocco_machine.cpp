#include "lamarzocco_machine.h"
#include "config.h"
#include "boiler_display.h"
#include "water_alarm.h"
#include "brewing_display.h"
#include "display_power.h"
#include "update_screen.h"
#include "command_feedback.h"
#include <ArduinoJson.h>
#include <WiFi.h>

namespace {

constexpr unsigned long RECONNECT_INITIAL_INTERVAL_MS = 10000;
constexpr unsigned long RECONNECT_MAX_INTERVAL_MS = 60000;
constexpr unsigned long DASHBOARD_QUIET_INTERVAL_MS = 45000;
constexpr unsigned long DASHBOARD_POLL_INTERVAL_MS = 30000;
constexpr unsigned long COMMAND_REFRESH_DELAY_MS = 1500;
constexpr unsigned long COMMAND_REFRESH_RETRY_INITIAL_MS = 5000;
constexpr unsigned long COMMAND_REFRESH_RETRY_MAX_MS = 30000;
constexpr uint8_t COMMAND_REFRESH_MAX_ATTEMPTS = 3;
constexpr unsigned long STATS_REFRESH_RETRY_INTERVAL_MS = 30000;
constexpr uint8_t CLOUD_FAILURES_BEFORE_NOTICE = 3;

bool machine_status_is_active(const char* status) {
    return status != nullptr &&
           strcmp(status, "Off") != 0 &&
           strcmp(status, "StandBy") != 0;
}

bool deadline_reached(unsigned long now, unsigned long deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

LaMarzoccoMachine* LaMarzoccoMachine::_instance = nullptr;

LaMarzoccoMachine::LaMarzoccoMachine(LaMarzoccoClient& client, LaMarzoccoWebSocket& websocket)
    : _client(client),
      _websocket(websocket),
      _power_state(false),
      _steam_state(false),
      _power_state_valid(false),
      _steam_state_valid(false),
      _command_queue(xQueueCreate(4, sizeof(QueuedCommand))),
      _pending_commands(0),
      _state_mux(portMUX_INITIALIZER_UNLOCKED) {
    _instance = this;
    _websocket.set_message_callback(_websocket_message_handler);

    if (!_command_queue) {
        debugln("Failed to create machine command queue");
    }
}

LaMarzoccoMachine::~LaMarzoccoMachine() {
    if (_instance == this) {
        _instance = nullptr;
    }

    if (_command_queue) {
        vQueueDelete(_command_queue);
        _command_queue = nullptr;
    }
}

void LaMarzoccoMachine::_websocket_message_handler(const String& message) {
    if (_instance && _instance->_handle_dashboard_message(message)) {
        _instance->_last_websocket_dashboard_ms = millis();
    }
}

bool LaMarzoccoMachine::_handle_dashboard_message(const String& message) {
    JsonDocument doc;

    Serial.println("\n========== DASHBOARD MESSAGE RECEIVED ==========");
    Serial.print("Message length: ");
    Serial.print(message.length());
    Serial.println(" bytes");

    DeserializationError error = deserializeJson(doc, message);
    if (error) {
        Serial.print("❌ JSON parse error: ");
        Serial.println(error.c_str());
        return false;
    }

    const bool has_widgets = doc["widgets"].is<JsonArray>();
    const bool has_removed_widgets =
        doc["removedWidgets"].is<JsonArray>();
    if (!has_widgets && !has_removed_widgets) {
        Serial.println(
            "⚠ Dashboard message has no widgets or removals");
        return false;
    }

    const char* machine_status = nullptr;
    const char* machine_mode = nullptr;
    const char* coffee_boiler_status = nullptr;
    int64_t coffee_ready_time = 0;
    float coffee_target_temp = 0.0f;
    const char* steam_boiler_status = nullptr;
    int64_t steam_ready_time = 0;
    const char* steam_target_level = nullptr;
    bool steam_enabled = false;
    bool steam_enabled_authoritative = false;
    bool no_water_alarm = false;
    bool water_alarm_authoritative = false;
    bool authoritative_update = false;
    bool is_brewing = false;
    int64_t brewing_start_time = 0;
    int64_t last_coffee_event_ms = 0;
    int64_t last_flush_event_ms = 0;

    // The API removes CMNoWater entirely when the alarm clears instead of
    // reliably sending {"allarm": false}. Treat its tombstone as an
    // authoritative clear; a current widget or NoWater boiler state below can
    // still override this back to active.
    if (has_removed_widgets) {
        for (JsonVariant removed :
             doc["removedWidgets"].as<JsonArray>()) {
            const char* removed_code = removed["code"];
            if (removed_code &&
                strcmp(removed_code, "CMNoWater") == 0) {
                no_water_alarm = false;
                water_alarm_authoritative = true;
                authoritative_update = true;
                break;
            }
        }
    }

    JsonArray widgets = doc["widgets"].as<JsonArray>();
    Serial.print("Widgets count: ");
    Serial.println(widgets.size());

    for (JsonVariant widget : widgets) {
        const char* code = widget["code"];
        if (!code) {
            continue;
        }

        JsonObject output = widget["output"].as<JsonObject>();
        if (strcmp(code, "CMMachineStatus") == 0) {
            machine_status = output["status"];
            authoritative_update =
                authoritative_update || machine_status != nullptr;
            machine_mode = output["mode"];
            is_brewing =
                machine_status && strcmp(machine_status, "Brewing") == 0;
            if (is_brewing && !output["brewingStartTime"].isNull()) {
                brewing_start_time =
                    output["brewingStartTime"].as<long long>();
            }
            if (output["lastCoffee"].is<JsonObject>() &&
                !output["lastCoffee"]["time"].isNull()) {
                last_coffee_event_ms =
                    output["lastCoffee"]["time"].as<long long>();
            }
            if (output["lastFlush"].is<JsonObject>() &&
                !output["lastFlush"]["time"].isNull()) {
                last_flush_event_ms =
                    output["lastFlush"]["time"].as<long long>();
            }
        } else if (strcmp(code, "CMCoffeeBoiler") == 0) {
            coffee_boiler_status = output["status"];
            authoritative_update =
                authoritative_update || coffee_boiler_status != nullptr;
            if (!output["readyStartTime"].isNull()) {
                coffee_ready_time =
                    output["readyStartTime"].as<long long>();
            }
            if (!output["targetTemperature"].isNull()) {
                coffee_target_temp =
                    output["targetTemperature"].as<float>();
            }
        } else if (strcmp(code, "CMSteamBoilerLevel") == 0) {
            steam_boiler_status = output["status"];
            authoritative_update =
                authoritative_update || steam_boiler_status != nullptr;
            if (!output["enabled"].isNull()) {
                steam_enabled = output["enabled"].as<bool>();
                steam_enabled_authoritative = true;
                authoritative_update = true;
            }
            if (!output["readyStartTime"].isNull()) {
                steam_ready_time =
                    output["readyStartTime"].as<long long>();
            }
            if (!output["targetLevel"].isNull()) {
                steam_target_level = output["targetLevel"];
            }
        } else if (strcmp(code, "CMNoWater") == 0 &&
                   !output["allarm"].isNull()) {
            no_water_alarm = output["allarm"].as<bool>();
            water_alarm_authoritative = true;
            authoritative_update = true;
        }
    }

    if (machine_status) {
        const bool powered_on = machine_status_is_active(machine_status);
        const bool was_powered_on =
            _machine_status_seen &&
            machine_status_is_active(_last_machine_status.c_str());

        _set_power_state(powered_on);
        if (powered_on && (!_machine_status_seen || !was_powered_on)) {
            // This is the machine-startup wake trigger. Initial "Off" snapshots
            // remain quiet; the first active state and Off->active both wake.
            display_power_mark_machine_activity();
        }
        _last_machine_status = machine_status;
        _machine_status_seen = true;

        Serial.print("📊 Machine status: ");
        Serial.print(machine_status);
        if (machine_mode) {
            Serial.print(" (mode: ");
            Serial.print(machine_mode);
            Serial.print(")");
        }
        Serial.println();

        const bool was_brewing =
            _last_brewing_state_valid && _last_brewing_state;
        if (!_last_brewing_state_valid ||
            is_brewing != _last_brewing_state) {
            _last_brewing_state = is_brewing;
            _last_brewing_state_valid = true;
            if (was_brewing && !is_brewing) {
                request_stats_refresh();
            }
        }

        // Partial/command-only frames must not terminate an active shot.
        brewing_display_update(is_brewing, brewing_start_time);

        // A complete shot or flush can happen while push updates are down and
        // between REST polls. The dashboard's last-event timestamps let the
        // counters catch up even when no live Brewing -> idle edge was seen.
        if (last_coffee_event_ms > 0 &&
            last_coffee_event_ms != _last_coffee_event_ms) {
            _last_coffee_event_ms = last_coffee_event_ms;
            request_stats_refresh();
        }
        if (last_flush_event_ms > 0 &&
            last_flush_event_ms != _last_flush_event_ms) {
            _last_flush_event_ms = last_flush_event_ms;
            request_stats_refresh();
        }
    }

    if (steam_enabled_authoritative) {
        _set_steam_state(steam_enabled);
    } else if (steam_boiler_status) {
        _set_steam_state(machine_status_is_active(steam_boiler_status));
    }

    if (coffee_boiler_status &&
        strcmp(coffee_boiler_status, "NoWater") == 0) {
        no_water_alarm = true;
        water_alarm_authoritative = true;
    }
    if (steam_boiler_status &&
        strcmp(steam_boiler_status, "NoWater") == 0) {
        no_water_alarm = true;
        water_alarm_authoritative = true;
    }
    if (water_alarm_authoritative) {
        // Do not clear an alarm merely because a partial frame omitted its
        // authoritative widget.
        water_alarm_set(no_water_alarm);
    }

    if (machine_status) {
        char coffee_temp_str[16] = "";
        char steam_level_str[16] = "";

        if (coffee_target_temp > 0.0f) {
            const float temp_f =
                (coffee_target_temp * 9.0f / 5.0f) + 32.0f;
            snprintf(coffee_temp_str,
                     sizeof(coffee_temp_str),
                     "%.1f°F",
                     temp_f);
        }

        if (steam_target_level) {
            if (strncmp(steam_target_level, "Level", 5) == 0) {
                snprintf(steam_level_str,
                         sizeof(steam_level_str),
                         "Level %s",
                         steam_target_level + 5);
            } else {
                snprintf(steam_level_str,
                         sizeof(steam_level_str),
                         "%s",
                         steam_target_level);
            }
        }

        const bool machine_off = !machine_status_is_active(machine_status);
        if (machine_off) {
            boiler_display_update(
                BOILER_COFFEE,
                machine_status,
                coffee_boiler_status ? coffee_boiler_status : "Off",
                coffee_ready_time,
                coffee_temp_str[0] ? coffee_temp_str : nullptr);
            boiler_display_update(
                BOILER_STEAM,
                machine_status,
                steam_boiler_status ? steam_boiler_status : "Off",
                steam_ready_time,
                steam_level_str[0] ? steam_level_str : nullptr);
        } else {
            if (coffee_boiler_status) {
                boiler_display_update(
                    BOILER_COFFEE,
                    machine_status,
                    coffee_boiler_status,
                    coffee_ready_time,
                    coffee_temp_str[0] ? coffee_temp_str : nullptr);
            }
            if (steam_boiler_status) {
                boiler_display_update(
                    BOILER_STEAM,
                    machine_status,
                    steam_boiler_status,
                    steam_ready_time,
                    steam_level_str[0] ? steam_level_str : nullptr);
            }
        }
    }

    if (doc["commands"].is<JsonArray>()) {
        for (JsonVariant command : doc["commands"].as<JsonArray>()) {
            const char* id = command["id"];
            const char* status = command["status"];
            if (id && status) {
                Serial.print("Command ");
                Serial.print(id);
                Serial.print(": ");
                Serial.println(status);
            }
        }
    }

    Serial.println("===============================================\n");
    if (authoritative_update) {
        _note_cloud_success();
    }
    return authoritative_update;
}

bool LaMarzoccoMachine::set_power(bool enabled) {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }
    
    JsonDocument request;
    request["mode"] = enabled ? "BrewingMode" : "StandBy";
    
    bool success = _client.api_call("POST", 
                                     "/things/" + serial + "/command/CoffeeMachineChangeMode",
                                     &request, nullptr);
    
    if (success) {
        _set_power_state(enabled);
        debug("Power set to: ");
        debugln(enabled ? "ON" : "OFF");
    } else {
        debugln("Failed to set power");
    }
    
    return success;
}

bool LaMarzoccoMachine::toggle_power() {
    return set_power(!get_power_state());
}

bool LaMarzoccoMachine::get_power_state() const {
    portENTER_CRITICAL(&_state_mux);
    bool state = _power_state;
    portEXIT_CRITICAL(&_state_mux);
    return state;
}

LaMarzoccoMachine::QueueResult LaMarzoccoMachine::queue_power_toggle() {
    return _queue_toggle(CommandType::Power);
}

bool LaMarzoccoMachine::set_steam(bool enabled) {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }
    
    JsonDocument request;
    request["boilerIndex"] = 1;  // Steam boiler index
    request["enabled"] = enabled;
    
    bool success = _client.api_call("POST", 
                                     "/things/" + serial + "/command/CoffeeMachineSettingSteamBoilerEnabled",
                                     &request, nullptr);
    
    if (success) {
        _set_steam_state(enabled);
        debug("Steam boiler set to: ");
        debugln(enabled ? "ON" : "OFF");
    } else {
        debugln("Failed to set steam boiler");
    }
    
    return success;
}

bool LaMarzoccoMachine::toggle_steam() {
    Serial.println("===========================================");
    Serial.println("STEAM BUTTON PRESSED - Toggling Steam Boiler");
    Serial.print("Current steam state: ");
    bool current_state = get_steam_state();
    Serial.println(current_state ? "ON" : "OFF");
    Serial.print("Target steam state: ");
    Serial.println(current_state ? "OFF" : "ON");
    Serial.println("===========================================");
    
    return set_steam(!current_state);
}

bool LaMarzoccoMachine::get_steam_state() const {
    portENTER_CRITICAL(&_state_mux);
    bool state = _steam_state;
    portEXIT_CRITICAL(&_state_mux);
    return state;
}

LaMarzoccoMachine::QueueResult LaMarzoccoMachine::queue_steam_toggle() {
    return _queue_toggle(CommandType::Steam);
}

bool LaMarzoccoMachine::connect_websocket() {
    if (is_websocket_connected()) {
        return true;  // Already connected
    }
    
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        debugln("Serial number not set");
        return false;
    }

    const bool started = _websocket.connect(serial);
    _connection_attempt_pending = started;
    if (!started) {
        _note_cloud_failure();
        // connect() may perform blocking authentication before it can start
        // the asynchronous transport. Back off from completion, not entry.
        _last_reconnect_attempt_ms = millis();
    }
    return started;
}

bool LaMarzoccoMachine::is_websocket_connected() const {
    return _websocket.is_connected();
}

void LaMarzoccoMachine::disconnect_websocket() {
    _websocket.disconnect();
}

void LaMarzoccoMachine::loop() {
    _websocket.loop();

    // Commands have priority over all background REST work. Process at most
    // one blocking operation per pass so the WebSocket is serviced between
    // requests and no two TLS users race each other.
    if (_process_next_command()) {
        return;
    }

    if (!WiFi.isConnected()) {
        return;
    }

    const unsigned long now = millis();
    if (is_websocket_connected()) {
        _connection_attempt_pending = false;
        _reconnect_interval_ms = RECONNECT_INITIAL_INTERVAL_MS;
        _last_reconnect_attempt_ms = now;
    } else if (_websocket.is_connecting()) {
        // Keep the loop hot during the authenticated WebSocket/STOMP
        // handshake; background HTTP here can consume most of its timeout.
        return;
    } else {
        if (_connection_attempt_pending) {
            _connection_attempt_pending = false;
            // The asynchronous TCP/WebSocket/STOMP attempt has just finished
            // unsuccessfully. Give it the full current backoff before trying
            // again, regardless of how long its handshake took.
            _last_reconnect_attempt_ms = now;
            _note_cloud_failure();
        }

        const bool reconnect_due =
            _last_reconnect_attempt_ms == 0 ||
            now - _last_reconnect_attempt_ms >= _reconnect_interval_ms;
        if (reconnect_due) {
            _last_reconnect_attempt_ms = now;
            Serial.println(
                "[AUTO-RECONNECT] Starting fresh authenticated WebSocket attempt");
            connect_websocket();
            _reconnect_interval_ms =
                min(_reconnect_interval_ms * 2UL,
                    RECONNECT_MAX_INTERVAL_MS);
            return;
        }
    }

    if (_stats_refresh_pending &&
        (_last_stats_refresh_ms == 0 ||
         now - _last_stats_refresh_ms >=
             STATS_REFRESH_RETRY_INTERVAL_MS)) {
        _stats_refresh_pending = !_refresh_shot_counters();
        // A timed-out GET may take longer than the retry interval. Timestamp
        // completion so failures cannot trigger a new blocking request on the
        // very next loop pass.
        _last_stats_refresh_ms = millis();
        return;
    }

    const bool command_refresh_due =
        _dashboard_refresh_pending &&
        deadline_reached(now, _dashboard_refresh_due_ms);
    const bool websocket_quiet =
        !is_websocket_connected() ||
        _last_websocket_dashboard_ms == 0 ||
        now - _last_websocket_dashboard_ms >=
            DASHBOARD_QUIET_INTERVAL_MS;
    const bool periodic_poll_due =
        websocket_quiet &&
        (_last_dashboard_poll_ms == 0 ||
         now - _last_dashboard_poll_ms >= DASHBOARD_POLL_INTERVAL_MS);

    if (command_refresh_due) {
        _last_dashboard_poll_ms = now;
        if (_refresh_dashboard()) {
            _dashboard_refresh_pending = false;
            _dashboard_refresh_attempts = 0;
        } else if (_dashboard_refresh_attempts <
                   COMMAND_REFRESH_MAX_ATTEMPTS) {
            _note_cloud_failure();
            ++_dashboard_refresh_attempts;
            const unsigned long retry_delay =
                min(COMMAND_REFRESH_RETRY_INITIAL_MS
                        << (_dashboard_refresh_attempts - 1),
                    COMMAND_REFRESH_RETRY_MAX_MS);
            _dashboard_refresh_due_ms = millis() + retry_delay;
        } else {
            _note_cloud_failure();
            _dashboard_refresh_pending = false;
            _dashboard_refresh_attempts = 0;
            debugln("Command dashboard reconciliation exhausted retries");
        }
        return;
    }

    if (periodic_poll_due) {
        _last_dashboard_poll_ms = now;
        if (!_refresh_dashboard()) {
            _note_cloud_failure();
        }
    }
}

LaMarzoccoMachine::QueueResult LaMarzoccoMachine::_queue_toggle(CommandType type) {
    if (!_command_queue) {
        return QueueResult::Unavailable;
    }

    uint8_t pending_bit =
        type == CommandType::Power ? POWER_PENDING_BIT : STEAM_PENDING_BIT;
    bool target_state = false;

    portENTER_CRITICAL(&_state_mux);
    if ((_pending_commands & pending_bit) != 0) {
        portEXIT_CRITICAL(&_state_mux);
        return QueueResult::AlreadyPending;
    }

    const bool state_valid =
        type == CommandType::Power
            ? _power_state_valid
            : _steam_state_valid;
    if (!state_valid) {
        portEXIT_CRITICAL(&_state_mux);
        debugln("Machine state not synchronized yet; toggle rejected");
        return QueueResult::Unavailable;
    }

    target_state =
        type == CommandType::Power ? !_power_state : !_steam_state;
    _pending_commands |= pending_bit;
    portEXIT_CRITICAL(&_state_mux);

    QueuedCommand command{type, target_state};
    if (xQueueSend(_command_queue, &command, 0) != pdTRUE) {
        portENTER_CRITICAL(&_state_mux);
        _pending_commands &= static_cast<uint8_t>(~pending_bit);
        portEXIT_CRITICAL(&_state_mux);
        return QueueResult::Unavailable;
    }

    return QueueResult::Queued;
}

bool LaMarzoccoMachine::_process_next_command() {
    if (!_command_queue) {
        return false;
    }

    QueuedCommand command;
    if (xQueueReceive(_command_queue, &command, 0) != pdTRUE) {
        return false;
    }

    bool success = false;
    uint8_t pending_bit = 0;
    CommandFeedbackTarget feedback_target = CommandFeedbackTarget::Power;

    if (command.type == CommandType::Power) {
        pending_bit = POWER_PENDING_BIT;
        feedback_target = CommandFeedbackTarget::Power;
        success = set_power(command.enabled);
    } else {
        pending_bit = STEAM_PENDING_BIT;
        feedback_target = CommandFeedbackTarget::Steam;
        success = set_steam(command.enabled);
    }

    portENTER_CRITICAL(&_state_mux);
    _pending_commands &= static_cast<uint8_t>(~pending_bit);
    portEXIT_CRITICAL(&_state_mux);

    if (success) {
        // A push normally confirms the command. This delayed authoritative
        // dashboard read covers a quiet or disconnected WebSocket.
        _dashboard_refresh_pending = true;
        _dashboard_refresh_due_ms = millis() + COMMAND_REFRESH_DELAY_MS;
        _dashboard_refresh_attempts = 0;
    }

    command_feedback_complete_from_worker(feedback_target, success);
    return true;
}

void LaMarzoccoMachine::_set_power_state(bool enabled) {
    portENTER_CRITICAL(&_state_mux);
    _power_state = enabled;
    _power_state_valid = true;
    portEXIT_CRITICAL(&_state_mux);
}

void LaMarzoccoMachine::_set_steam_state(bool enabled) {
    portENTER_CRITICAL(&_state_mux);
    _steam_state = enabled;
    _steam_state_valid = true;
    portEXIT_CRITICAL(&_state_mux);
}

void LaMarzoccoMachine::_note_cloud_failure() {
    if (_consecutive_cloud_failures < UINT8_MAX) {
        ++_consecutive_cloud_failures;
    }

    portENTER_CRITICAL(&_state_mux);
    const bool state_synchronized =
        _power_state_valid && _steam_state_valid;
    portEXIT_CRITICAL(&_state_mux);

    if (!state_synchronized &&
        _consecutive_cloud_failures >=
            CLOUD_FAILURES_BEFORE_NOTICE) {
        showCloudConnectionScreen();
    }
}

void LaMarzoccoMachine::_note_cloud_success() {
    portENTER_CRITICAL(&_state_mux);
    const bool controls_synchronized =
        _power_state_valid && _steam_state_valid;
    portEXIT_CRITICAL(&_state_mux);

    // A partial dashboard proves that some cloud traffic is flowing, but it
    // must not dismiss the recovery notice while either control is still
    // unusable because its authoritative state has never arrived.
    if (controls_synchronized) {
        _consecutive_cloud_failures = 0;
        clearCloudConnectionScreen();
    }
}

void LaMarzoccoMachine::request_stats_refresh() {
    _stats_refresh_pending = true;
    _last_stats_refresh_ms = 0;
}

bool LaMarzoccoMachine::_refresh_shot_counters() {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        return false;
    }

    JsonDocument response;
    String endpoint = "/things/" + serial + "/stats/COFFEE_AND_FLUSH_COUNTER/1";
    if (!_client.api_call("GET", endpoint, nullptr, &response)) {
        debugln("Failed to fetch coffee/flush counters");
        return false;
    }

    if (!response["output"].is<JsonObject>()) {
        debugln("Coffee/flush counter response missing output object");
        return false;
    }

    JsonObject output = response["output"].as<JsonObject>();
    JsonVariant total_coffee_value = output["totalCoffee"];
    JsonVariant total_flush_value = output["totalFlush"];
    if (!total_coffee_value.is<uint32_t>() ||
        !total_flush_value.is<uint32_t>()) {
        debugln("Coffee/flush counter response has invalid counters");
        return false;
    }

    uint32_t total_coffee = total_coffee_value.as<uint32_t>();
    uint32_t total_flush = total_flush_value.as<uint32_t>();
    return updateShotCounters(total_coffee, total_flush);
}

bool LaMarzoccoMachine::_refresh_dashboard() {
    String serial = _client.get_serial_number();
    if (serial.length() == 0) {
        return false;
    }

    JsonDocument response;
    String endpoint = "/things/" + serial + "/dashboard";
    if (!_client.api_call("GET", endpoint, nullptr, &response)) {
        debugln("Dashboard fallback request failed");
        return false;
    }

    if (!response["widgets"].is<JsonArray>()) {
        debugln("Dashboard fallback response missing widgets");
        return false;
    }

    String message;
    serializeJson(response, message);
    debugln("Applying REST dashboard fallback");
    return _handle_dashboard_message(message);
}
