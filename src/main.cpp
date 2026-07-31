#include <Arduino.h>
#include <LilyGo_AMOLED.h>
#include <LV_Helper.h>
#include <ui/ui.h>
#include "Preferences.h"
#include "web.h"
#include <WiFi.h>
#include "config.h"
#include "update_screen.h"
#include "lamarzocco_client.h"
#include "lamarzocco_websocket.h"
#include "lamarzocco_machine.h"
#include "lamarzocco_auth.h"
#include "boiler_display.h"
#include "water_alarm.h"
#include "brewing_display.h"
#include "display_power.h"
#include "ui_theme.h"

#if __has_include("local_credentials.h")
#include "local_credentials.h"
#else
#define LOCAL_CREDENTIALS_ENABLED 0
#endif

Preferences preferences;
LaMarzoccoClient* g_client = nullptr;
LaMarzoccoWebSocket* g_websocket = nullptr;
LaMarzoccoMachine* g_machine = nullptr;

LilyGo_Class amoled;
SemaphoreHandle_t gui_mutex;
SemaphoreHandle_t ui_ready_semaphore;
static volatile bool ui_ready = false;
void Task_LVGL(void *pvParameters);
void updateSerialLoggingPowerState(bool force);

static bool loadScreenThreadSafe(lv_obj_t *screen)
{
  if (!ui_ready || !screen || !gui_mutex) {
    return false;
  }

  if (xSemaphoreTake(gui_mutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  lv_disp_load_scr(screen);
  xSemaphoreGive(gui_mutex);
  return true;
}

// WiFi connection variables
const int MAX_WIFI_RETRIES = 10;
const int WIFI_TIMEOUT_MS = 15000;

static void storePreferenceIfChanged(const char *key, const char *value)
{
  if (!value || value[0] == '\0') {
    return;
  }

  if (preferences.getString(key, "") != value) {
    preferences.putString(key, value);
  }
}

static void applyLocalCredentials()
{
#if LOCAL_CREDENTIALS_ENABLED
  const bool has_local_wifi =
      LOCAL_WIFI_SSID[0] != '\0' && LOCAL_WIFI_PASSWORD[0] != '\0';
  const bool has_saved_wifi =
      preferences.getString("SSID", "").length() > 0 &&
      preferences.getString("PASS", "").length() > 0;

  // Preserve a working captive-portal configuration. The local WiFi values
  // are recovery defaults only, so changing networks remains possible.
  if (has_local_wifi && !has_saved_wifi) {
    storePreferenceIfChanged("SSID", LOCAL_WIFI_SSID);
    storePreferenceIfChanged("PASS", LOCAL_WIFI_PASSWORD);
    Serial.println("[CONFIG] Restored missing WiFi credentials");
  }

  // A local private build is authoritative for the cloud account. This also
  // repairs stale or partially saved portal values on every boot.
  storePreferenceIfChanged("USER_EMAIL", LOCAL_LM_EMAIL);
  storePreferenceIfChanged("USER_PASS", LOCAL_LM_PASSWORD);
  storePreferenceIfChanged("MACHINE", LOCAL_MACHINE_SERIAL);
  Serial.println("[CONFIG] Applied local cloud credentials");
#endif
}

bool connectToWiFi(const String &ssid, const String &password)
{
  Serial.print("[WIFI] Connecting to saved network: ");
  Serial.println(ssid);
  WiFi.begin(ssid.c_str(), password.c_str());
  WiFi.setSleep(false);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < MAX_WIFI_RETRIES)
  {
    delay(WIFI_TIMEOUT_MS / MAX_WIFI_RETRIES);
    debug(".");
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("[WIFI] Connected");
    Serial.print("[WIFI] IP address: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  else
  {
    Serial.println(
      "[WIFI] Saved network did not connect within 15 seconds; "
      "credentials retained");
    WiFi.disconnect();
    return false;
  }
}

static bool initializeMachineFromStoredCredentials(bool connectImmediately)
{
  if (g_machine) {
    return true;
  }

  String email = preferences.getString("USER_EMAIL", "");
  String password = preferences.getString("USER_PASS", "");
  String machine_serial = preferences.getString("MACHINE", "");
  if (email.length() == 0 ||
      password.length() == 0 ||
      machine_serial.length() == 0) {
    Serial.println("[CLOUD] Stored account configuration is incomplete");
    return false;
  }

  InstallationKey key;
  if (!LaMarzoccoAuth::load_installation_key(preferences, key)) {
    Serial.println("[CLOUD] Generating a new installation key");

    // Clear only incomplete/legacy installation-key material. WiFi and account
    // credentials use different keys and are never touched here.
    if (preferences.isKey("INSTALLATION_ID")) preferences.remove("INSTALLATION_ID");
    if (preferences.isKey("INSTALLATION_SECRET")) preferences.remove("INSTALLATION_SECRET");
    if (preferences.isKey("INSTALLATION_PRIVKEY")) preferences.remove("INSTALLATION_PRIVKEY");
    if (preferences.isKey("INSTALLATION_PUBKEY")) preferences.remove("INSTALLATION_PUBKEY");
    if (preferences.isKey("INSTALLATION_PRIVKEY_LEN")) preferences.remove("INSTALLATION_PRIVKEY_LEN");
    if (preferences.isKey("INSTALLATION_PUBKEY_LEN")) preferences.remove("INSTALLATION_PUBKEY_LEN");
    if (preferences.isKey("INST_ID")) preferences.remove("INST_ID");
    if (preferences.isKey("INST_SECRET")) preferences.remove("INST_SECRET");
    if (preferences.isKey("INST_PRIVKEY")) preferences.remove("INST_PRIVKEY");
    if (preferences.isKey("INST_PUBKEY")) preferences.remove("INST_PUBKEY");
    if (preferences.isKey("INST_PRIVLEN")) preferences.remove("INST_PRIVLEN");
    if (preferences.isKey("INST_PUBLEN")) preferences.remove("INST_PUBLEN");

    String installation_id = LaMarzoccoAuth::generate_uuid();
    if (!LaMarzoccoAuth::generate_installation_key(installation_id, key) ||
        !LaMarzoccoAuth::save_installation_key(preferences, key)) {
      Serial.println("[CLOUD] Failed to create installation key");
      showNoConnectionScreen(
        "Client Init Failed!\n"
        "Please restart WiFi Setup"
      );
      setupWEB();
      return false;
    }
  }

  g_client = new LaMarzoccoClient(preferences);
  if (!g_client ||
      !g_client->init(email, password, machine_serial)) {
    Serial.println("[CLOUD] Failed to initialize client");
    delete g_client;
    g_client = nullptr;
    showNoConnectionScreen(
      "Client Init Failed!\n"
      "Please restart WiFi Setup"
    );
    setupWEB();
    return false;
  }

  g_websocket = new LaMarzoccoWebSocket(*g_client);
  if (!g_websocket) {
    delete g_client;
    g_client = nullptr;
    return false;
  }

  g_machine = new LaMarzoccoMachine(*g_client, *g_websocket);
  if (!g_machine) {
    delete g_websocket;
    g_websocket = nullptr;
    delete g_client;
    g_client = nullptr;
    return false;
  }

  Serial.println("[CLOUD] La Marzocco client initialized");
  g_machine->request_stats_refresh();

  if (connectImmediately) {
    Serial.println("[CLOUD] Starting WebSocket connection");
    if (!g_machine->connect_websocket()) {
      Serial.println(
        "[CLOUD] Initial connection unavailable; automatic retry is active");
    }
  }

  return true;
}

void updateSerialLoggingPowerState(bool force)
{
  static bool serial_enabled = true;
  static unsigned long last_check_ms = 0;
  unsigned long now = millis();

  if (!force && (now - last_check_ms) < 5000) {
    return;
  }
  last_check_ms = now;

  bool usb_powered = amoled.isVbusIn();
  if (usb_powered && !serial_enabled) {
    Serial.begin(115200);
    serial_enabled = true;
  } else if (!usb_powered && serial_enabled) {
    Serial.flush();
    Serial.end();
    serial_enabled = false;
  }
}

void setup()
{
  Serial.begin(115200);
  preferences.begin("config", false);
  applyLocalCredentials();
  pinMode(0, INPUT_PULLUP);

  bool rslt = false;

  // Automatically determine the access device
  rslt = amoled.begin();

  if (!rslt)
  {
    while (1)
    {
      debug("The board model cannot be detected, please raise the Core Debug Level to an error");
      delay(1000);
    }
  }

  if (!display_power_init(&amoled,
                          DISPLAY_BRIGHTNESS_ACTIVE,
                          DISPLAY_BRIGHTNESS_DIM,
                          USER_DIM_TIMEOUT_MS)) {
    Serial.println("Display power manager initialization failed");
  }

  updateSerialLoggingPowerState(true);

  gui_mutex = xSemaphoreCreateMutex();
  ui_ready_semaphore = xSemaphoreCreateBinary();
  if (gui_mutex == NULL || ui_ready_semaphore == NULL)
  {
    log_i("UI synchronization primitive creation failure");
    return;
  }

  BaseType_t task_created =
      xTaskCreatePinnedToCore(Task_LVGL,
                             "Task_LVGL",
                             1024 * 16,
                             NULL,
                             3,
                             NULL,
                             0);
  if (task_created != pdPASS) {
    log_i("LVGL task creation failure");
    return;
  }

  // No screen object may be accessed from the network/setup core until LVGL
  // has created the complete UI and its supporting display modules.
  xSemaphoreTake(ui_ready_semaphore, portMAX_DELAY);

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  delay(500);

  String ssid = preferences.getString("SSID", "");
  String pass = preferences.getString("PASS", "");
  if (ssid == "" || pass == "")
  {
    Serial.println("[WIFI] No saved credentials");
    showNoConnectionScreen(
      "No Saved WiFi\n"
      "Please start WiFi Setup"
    );
    setupWEB();
  }
  else
  {
    Serial.println("[WIFI] Saved credentials found");
    setWiFiRecoveryCredentials(ssid, pass);
    // Arm recovery before any blocking cloud startup work can run. If the
    // initial association succeeds, the first monitor pass simply clears this
    // provisional state.
    startWiFiRecovery(false);
    const bool wifi_connected = connectToWiFi(ssid, pass);

    if (wifi_connected) {
      loadScreenThreadSafe(ui_mainScreen);
    } else {
      showNoConnectionScreen(
        "Saved WiFi Unavailable\n"
        "Retrying automatically"
      );
      setupWEB();
      startWiFiRecovery(true);
    }

    // Client construction is local-only. Creating it even while WiFi is down
    // lets the normal machine loop authenticate automatically after recovery.
    initializeMachineFromStoredCredentials(wifi_connected);
  }
}

void loop()
{
  serviceWEBSetupRequest();
  updateDateTime();
  updateStatusImages();  // Update battery and WiFi images (initial + every 30 seconds)
  checkWiFiConnection(); // Monitor WiFi connection and redirect if disconnected
  updateSerialLoggingPowerState(false);
  
  // Check GPIO 15 for brewing simulation mode
  brewing_display_check_gpio_simulation();
  
  // Handle websocket and machine loop - MUST be called frequently
  // WebSocket requires regular loop() calls to process messages
  if (g_machine && !isWEBActive()) {
    g_machine->loop();  // This calls websocket.loop()
  }
  
  // Small delay to prevent watchdog issues, but keep loop responsive
  delay(10);

  // The BOOT button is now only an optional display wake source. Deep sleep is
  // intentionally disabled so Wi-Fi and machine events remain available.
  static bool boot_was_pressed = false;
  const bool boot_pressed = digitalRead(0) == LOW;
  if (boot_pressed && !boot_was_pressed) {
    display_power_mark_user_activity();
  }
  boot_was_pressed = boot_pressed;
}

void Task_LVGL(void *pvParameters)
{
  beginLvglHelper(amoled);
  ui_init();
  ui_theme_apply();
  
  // Initialize boiler display system after UI is ready
  boiler_display_set_mutex((void*)gui_mutex);  // Set mutex for thread-safe LVGL access
  boiler_display_init();
  
  // Initialize water alarm display system
  water_alarm_set_mutex((void*)gui_mutex);
  water_alarm_init();
  
  // Initialize brewing display system
  brewing_display_set_mutex((void*)gui_mutex);
  brewing_display_init();

  ui_ready = true;
  xSemaphoreGive(ui_ready_semaphore);
  
  // Main LVGL loop
  while (1)
  {
    if (xSemaphoreTake(gui_mutex, portMAX_DELAY) == pdTRUE)
    {
      // Brightness changes share this task with panel flushes, preventing QSPI
      // display traffic from racing a redraw on the other ESP32 core.
      display_power_loop();
      lv_timer_handler();
      xSemaphoreGive(gui_mutex);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
