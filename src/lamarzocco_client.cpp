#include "lamarzocco_client.h"
#include "config.h"
#include <time.h>

static const char* CUSTOMER_APP_URL = "https://lion.lamarzocco.io/api/customer-app";
static const unsigned long TOKEN_TIME_TO_REFRESH = 10 * 60;  // 10 minutes
static const unsigned long TOKEN_FALLBACK_LIFETIME_SEC = 60 * 60;
static const int32_t HTTP_CONNECT_TIMEOUT_MS = 8000;
static const uint16_t HTTP_READ_TIMEOUT_MS = 12000;

namespace {

unsigned long current_time_seconds() {
    // time() is non-blocking. Before NTP synchronization it returns a value near
    // zero, so use the monotonic clock as the temporary token-expiry time base.
    time_t now = time(nullptr);
    if (now > 1000000000) {
        return static_cast<unsigned long>(now);
    }
    return millis() / 1000;
}

bool is_success_status(int http_code) {
    return http_code >= 200 && http_code < 300;
}

}  // namespace

LaMarzoccoClient::LaMarzoccoClient(Preferences& prefs)
    : _prefs(prefs),
      _initialized(false),
      _registered(false),
      _request_mutex(xSemaphoreCreateMutex()) {
    // Keep the project's existing TLS behavior. Changing certificate
    // verification needs a separate, device-tested migration.
    _client.setInsecure();
}

LaMarzoccoClient::~LaMarzoccoClient() {
    if (_request_mutex != nullptr) {
        vSemaphoreDelete(_request_mutex);
        _request_mutex = nullptr;
    }
}

bool LaMarzoccoClient::_take_request_lock() const {
    return _request_mutex == nullptr ||
           xSemaphoreTake(_request_mutex, portMAX_DELAY) == pdTRUE;
}

void LaMarzoccoClient::_release_request_lock() const {
    if (_request_mutex != nullptr) {
        xSemaphoreGive(_request_mutex);
    }
}

void LaMarzoccoClient::_configure_http(HTTPClient& http) {
    http.setReuse(false);
    http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
    http.setTimeout(HTTP_READ_TIMEOUT_MS);
}

bool LaMarzoccoClient::init(const String& username,
                            const String& password,
                            const String& serial_number) {
    _username = username;
    _password = password;
    _serial_number = serial_number;
    _registered = false;

    // Load installation key
    if (!LaMarzoccoAuth::load_installation_key(_prefs, _installation_key)) {
        debugln("No installation key found, need to generate one");
        return false;
    }

    _initialized = true;
    return true;
}

bool LaMarzoccoClient::get_installation_key(InstallationKey& key) const {
    if (!_initialized) {
        return false;
    }
    key = _installation_key;
    return true;
}

String LaMarzoccoClient::get_access_token_string() const {
    if (!_take_request_lock()) {
        return "";
    }
    String token = _access_token.access_token;
    _release_request_lock();
    return token;
}

void LaMarzoccoClient::invalidate_access_token() {
    if (!_take_request_lock()) {
        return;
    }
    _access_token.access_token = "";
    _access_token.expires_at = 0;
    _release_request_lock();
}

bool LaMarzoccoClient::register_client() {
    if (!_take_request_lock()) {
        return false;
    }
    bool result = _register_client_unlocked();
    _release_request_lock();
    return result;
}

bool LaMarzoccoClient::_register_client_unlocked() {
    if (!_initialized) {
        debugln("Client not initialized");
        return false;
    }

    String base_string = LaMarzoccoAuth::generate_base_string(_installation_key);
    String proof = LaMarzoccoAuth::generate_request_proof(base_string, _installation_key.secret);
    String public_key_b64 = LaMarzoccoAuth::base64_encode(
        _installation_key.public_key_der,
        _installation_key.public_key_len);

    HTTPClient http;
    if (!http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/init")) {
        debugln("Registration failed: could not initialize HTTP client");
        return false;
    }
    _configure_http(http);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-App-Installation-Id", _installation_key.installation_id);
    http.addHeader("X-Request-Proof", proof);

    JsonDocument request;
    request["pk"] = public_key_b64;

    String request_body;
    serializeJson(request, request_body);

    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    Serial.printf("[CLOUD][AUTH] /auth/init -> %d\n", http_code);

    if (is_success_status(http_code)) {
        _registered = true;
        debugln("Registration successful");
        return true;
    }

    debug("Registration failed: ");
    debugln(http_code);
    debugln(response);
    return false;
}

bool LaMarzoccoClient::_parse_token_response(const String& response,
                                             bool refresh_response) {
    JsonDocument response_doc;
    DeserializationError json_error = deserializeJson(response_doc, response);
    if (json_error) {
        debug("Authentication returned invalid JSON: ");
        debugln(json_error.c_str());
        return false;
    }

    String access_token = response_doc["accessToken"].as<String>();
    if (access_token.length() == 0) {
        access_token = response_doc["access_token"].as<String>();
    }
    if (access_token.length() == 0) {
        Serial.println(
            "[CLOUD][AUTH] Successful response did not contain an access token");
        return false;
    }

    String refresh_token = response_doc["refreshToken"].as<String>();
    if (refresh_token.length() == 0) {
        refresh_token = response_doc["refresh_token"].as<String>();
    }

    // The live customer-app response does not consistently publish an expiry
    // field. The previously working client accepted the token regardless, and
    // current implementations refresh it on a conservative one-hour schedule.
    unsigned long expires_in =
        response_doc["expiresIn"].as<unsigned long>();
    if (expires_in == 0) {
        expires_in = response_doc["expires_in"].as<unsigned long>();
    }
    if (expires_in == 0) {
        expires_in = TOKEN_FALLBACK_LIFETIME_SEC;
    }

    // Only publish a new token after the complete response has been validated.
    _access_token.access_token = access_token;
    if (refresh_token.length() > 0) {
        _access_token.refresh_token = refresh_token;
    } else if (!refresh_response) {
        _access_token.refresh_token = "";
    }
    _access_token.expires_at = current_time_seconds() + expires_in;
    // A valid token proves this installation is known to the service. This
    // also prevents a transient/duplicate /auth/init response from being
    // retried before every otherwise-valid API request.
    _registered = true;
    return true;
}

bool LaMarzoccoClient::_sign_in() {
    JsonDocument request;
    request["username"] = _username;
    request["password"] = _password;

    String request_body;
    serializeJson(request, request_body);

    HTTPClient http;
    if (!http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/signin")) {
        debugln("Sign in failed: could not initialize HTTP client");
        return false;
    }
    _configure_http(http);
    http.addHeader("Content-Type", "application/json");
    _add_auth_headers(http);

    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    Serial.printf("[CLOUD][AUTH] /auth/signin -> %d\n", http_code);

    if (is_success_status(http_code) &&
        _parse_token_response(response, false)) {
        debugln("Sign in successful");
        return true;
    }

    debug("Sign in failed: ");
    debugln(http_code);
    if (!is_success_status(http_code)) {
        debugln(response);
    }
    return false;
}

bool LaMarzoccoClient::_refresh_token() {
    if (_access_token.refresh_token.length() == 0) {
        return _sign_in();
    }

    JsonDocument request;
    request["username"] = _username;
    request["refreshToken"] = _access_token.refresh_token;

    String request_body;
    serializeJson(request, request_body);

    HTTPClient http;
    if (!http.begin(_client, String(CUSTOMER_APP_URL) + "/auth/refreshtoken")) {
        debugln("Token refresh failed: could not initialize HTTP client");
        return _sign_in();
    }
    _configure_http(http);
    http.addHeader("Content-Type", "application/json");
    _add_auth_headers(http);

    int http_code = http.POST(request_body);
    String response = http.getString();
    http.end();
    Serial.printf("[CLOUD][AUTH] /auth/refreshtoken -> %d\n", http_code);

    if (is_success_status(http_code) &&
        _parse_token_response(response, true)) {
        debugln("Token refresh successful");
        return true;
    }

    debug("Token refresh failed: ");
    debugln(http_code);
    if (!is_success_status(http_code)) {
        debugln(response);
    }

    // A stale or revoked refresh token should not strand the device.
    return _sign_in();
}

bool LaMarzoccoClient::get_access_token() {
    if (!_take_request_lock()) {
        return false;
    }
    bool result = _get_access_token_unlocked();
    _release_request_lock();
    return result;
}

bool LaMarzoccoClient::_get_access_token_unlocked() {
    if (!_initialized) {
        return false;
    }

    // Registration is idempotent for an installation ID. Retrying it here
    // makes a transient boot-time timeout recover on the normal auth backoff
    // instead of stranding the device until a manual restart.
    if (!_registered) {
        // Signing in may still succeed when this installation was registered
        // during an earlier boot but the idempotent registration request is
        // temporarily unavailable (or the service reports "already exists").
        // Keep retrying registration on later auth attempts until it succeeds.
        _register_client_unlocked();
    }

    unsigned long now = current_time_seconds();
    bool token_valid = _access_token.access_token.length() > 0 &&
                       _access_token.expires_at > now;

    if (!token_valid ||
        _access_token.expires_at < now + TOKEN_TIME_TO_REFRESH) {
        if (_access_token.refresh_token.length() > 0) {
            return _refresh_token();
        }
        return _sign_in();
    }

    return true;
}

bool LaMarzoccoClient::_force_reauthenticate() {
    // Do not allow the normal validity check to return the token that the API
    // has just rejected. Refresh once, falling back to a direct sign-in.
    _access_token.access_token = "";
    _access_token.expires_at = 0;
    if (_access_token.refresh_token.length() > 0) {
        return _refresh_token();
    }
    return _sign_in();
}

void LaMarzoccoClient::_add_auth_headers(HTTPClient& http) {
    String installation_id, timestamp, nonce, signature;
    LaMarzoccoAuth::generate_extra_request_headers(
        _installation_key,
        installation_id,
        timestamp,
        nonce,
        signature);

    http.addHeader("X-App-Installation-Id", installation_id);
    http.addHeader("X-Timestamp", timestamp);
    http.addHeader("X-Nonce", nonce);
    http.addHeader("X-Request-Signature", signature);
}

bool LaMarzoccoClient::_api_call_once(const String& method,
                                      const String& url,
                                      JsonDocument* request_body,
                                      JsonDocument* response_body,
                                      int& http_code) {
    HTTPClient http;
    http_code = 0;
    if (!http.begin(_client, url)) {
        debugln("API call failed: could not initialize HTTP client");
        return false;
    }
    _configure_http(http);
    http.addHeader("Content-Type", "application/json");

    // These values must be generated for every attempt because the nonce and
    // timestamp are part of the request signature.
    _add_auth_headers(http);
    http.addHeader("Authorization", "Bearer " + _access_token.access_token);

    String request_str;
    if (request_body != nullptr) {
        serializeJson(*request_body, request_str);
    }

    if (method == "GET") {
        http_code = http.GET();
    } else if (method == "POST") {
        http_code = http.POST(request_str);
    } else if (method == "PUT") {
        http_code = http.PUT(request_str);
    } else if (method == "DELETE") {
        http_code = http.sendRequest("DELETE", request_str);
    } else {
        http.end();
        return false;
    }

    String response_str = http.getString();
    http.end();

    if (!is_success_status(http_code)) {
        debug("API call failed: ");
        debugln(http_code);
        debugln(response_str);
        return false;
    }

    if (response_body != nullptr) {
        response_body->clear();
        if (response_str.length() == 0) {
            debugln("API call returned an empty response where JSON was expected");
            return false;
        }

        DeserializationError json_error =
            deserializeJson(*response_body, response_str);
        if (json_error) {
            debug("API call returned invalid JSON: ");
            debugln(json_error.c_str());
            return false;
        }
    }

    return true;
}

bool LaMarzoccoClient::api_call(const String& method,
                                const String& endpoint,
                                JsonDocument* request_body,
                                JsonDocument* response_body) {
    if (method != "GET" && method != "POST" &&
        method != "PUT" && method != "DELETE") {
        debugln("Unsupported API method");
        return false;
    }

    if (!_take_request_lock()) {
        return false;
    }

    bool success = false;
    do {
        if (!_get_access_token_unlocked()) {
            break;
        }

        String url = String(CUSTOMER_APP_URL) + endpoint;
        int http_code = 0;
        if (_api_call_once(method,
                           url,
                           request_body,
                           response_body,
                           http_code)) {
            success = true;
            break;
        }

        if (http_code != 401) {
            break;
        }

        debugln("API authorization expired; refreshing and retrying once");
        if (!_force_reauthenticate()) {
            debugln("API retry cancelled because reauthentication failed");
            break;
        }

        // This is deliberately a single, non-recursive retry. A second 401 or
        // any other failure is returned to the caller.
        int retry_http_code = 0;
        success = _api_call_once(method,
                                 url,
                                 request_body,
                                 response_body,
                                 retry_http_code);
    } while (false);

    _release_request_lock();
    return success;
}
