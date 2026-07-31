#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "lamarzocco_auth.h"
#include "Preferences.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct AccessToken {
    String access_token;
    String refresh_token;
    unsigned long expires_at;  // Unix timestamp in seconds
};

class LaMarzoccoClient {
public:
    LaMarzoccoClient(Preferences& prefs);
    ~LaMarzoccoClient();
    
    // Initialize client with credentials
    bool init(const String& username, const String& password, const String& serial_number);
    
    // Register client (call after generating installation key)
    bool register_client();
    
    // Get access token (sign in or refresh)
    bool get_access_token();
    
    // Make authenticated API call
    bool api_call(const String& method, const String& endpoint, JsonDocument* request_body, JsonDocument* response_body);
    
    // Get installation key
    bool get_installation_key(InstallationKey& key) const;
    
    // Check if initialized
    bool is_initialized() const { return _initialized; }
    
    // Get serial number
    String get_serial_number() const { return _serial_number; }
    
    // Get access token string (for websocket)
    String get_access_token_string() const;

    // Force the next authenticated operation to refresh/sign in again.
    void invalidate_access_token();
    
private:
    Preferences& _prefs;
    InstallationKey _installation_key;
    AccessToken _access_token;
    String _username;
    String _password;
    String _serial_number;
    bool _initialized;
    bool _registered;
    WiFiClientSecure _client;
    SemaphoreHandle_t _request_mutex;
    
    // Internal helpers
    bool _register_client_unlocked();
    bool _get_access_token_unlocked();
    bool _sign_in();
    bool _refresh_token();
    bool _force_reauthenticate();
    bool _api_call_once(const String& method,
                        const String& url,
                        JsonDocument* request_body,
                        JsonDocument* response_body,
                        int& http_code);
    bool _parse_token_response(const String& response, bool refresh_response);
    bool _take_request_lock() const;
    void _release_request_lock() const;
    void _configure_http(HTTPClient& http);
    void _add_auth_headers(HTTPClient& http);
};
