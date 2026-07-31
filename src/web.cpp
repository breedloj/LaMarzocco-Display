#include <WiFi.h>
#include <WiFiClient.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "web_handle.h"

uint64_t timer = 0;

// DNS server
const byte DNS_PORT = 53;
DNSServer dnsServer;
WebServer server(80);
static volatile bool webTaskRunning = false;
static volatile bool webServerActive = false;
static volatile bool webStartRequested = false;
static bool routesRegistered = false;
static TaskHandle_t webTaskHandle = nullptr;

void setupAP()
{
    log_i("Configuring access point...");
    // Preserve the station connection/retry path while the setup hotspot is
    // available as a fallback.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID);
    WiFi.setSleep(false);
    delay(100);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    if (!MDNS.begin(AP_SSID)) // using same name as SSID, shottimer.local
        debugln("Error setting up MDNS responder!");
    else
        debugln("mDNS responder started");
    log_i("The hotspot has been established");
}

void webTask(void *args)
{
    (void)args;
    while (webTaskRunning)
    {
        dnsServer.processNextRequest();
        server.handleClient();
        vTaskDelay(10); // allow the cpu to switch to other tasks
    }

    // Close the resources from their owning task so stopWEB() never races an
    // in-flight HTTP request or network scan.
    server.stop();
    dnsServer.stop();
    MDNS.end();
    WiFi.softAPdisconnect(false);
    WiFi.enableAP(false);
    webServerActive = false;
    webTaskHandle = nullptr;
    vTaskDelete(NULL);
}

void setupWEB(void)
{
    if (webServerActive) {
        return;
    }

    setupAP();
    initFS();

    if (!routesRegistered) {
        server.on("/styles.css", HTTP_GET, cssHandler);
        server.on("/", HTTP_GET, mainHandler);
        server.on("/ssids", HTTP_GET, sendSSID);
        server.on("/statusData", HTTP_GET, sendStatus);
        server.on("/wifiConfig", HTTP_POST, saveWifiHandler);
        server.on("/cloudConfig", HTTP_POST, saveCloudHandler);
        server.on("/machineConfig", HTTP_POST, saveMachineHandler);
        server.on("/restart", HTTP_GET, restartHander);
        server.onNotFound(handleNotFound);
        routesRegistered = true;
    }

    server.begin();
    log_i("HTTP server started");
    timer = millis();

    webTaskRunning = true;
    BaseType_t created =
        xTaskCreatePinnedToCore(webTask,
                               "webTask",
                               16384,
                               nullptr,
                               1,
                               &webTaskHandle,
                               1);
    if (created != pdPASS) {
        webTaskRunning = false;
        webTaskHandle = nullptr;
        server.stop();
        dnsServer.stop();
        MDNS.end();
        WiFi.softAPdisconnect(false);
        WiFi.enableAP(false);
        log_e("Failed to start WiFi setup web task");
        return;
    }

    webServerActive = true;
}

void stopWEB(void)
{
    if (!webServerActive) {
        return;
    }

    // The web task performs its own orderly cleanup after its current handler
    // returns.
    webTaskRunning = false;
}

bool isWEBActive(void)
{
    return webServerActive;
}

void requestWEBSetup(void)
{
    // Safe to call from an LVGL event. Networking and task creation remain on
    // the Arduino main task.
    webStartRequested = true;
}

void serviceWEBSetupRequest(void)
{
    if (!webStartRequested) {
        return;
    }

    if (webServerActive) {
        // An active portal already satisfies the request. If it is currently
        // shutting down, retain the request and restart it after cleanup.
        if (webTaskRunning) {
            webStartRequested = false;
        }
        return;
    }

    webStartRequested = false;
    setupWEB();
}
