#include "wifi_sta.h"
#include <atomic>
#include <cstring>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

static const char* TAG = "JWIFI";
static std::atomic<bool> _connected{false};
static std::atomic<bool> _started{false};

static void onEvent(void*, esp_event_base_t base, int32_t id, void*)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        _connected = false;
        ESP_LOGW(TAG, "wifi weg, opnieuw verbinden");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        _connected = true;
        ESP_LOGI(TAG, "wifi verbonden");
    }
}

namespace jarvis {

void wifiStart(const char* ssid, const char* password)
{
    if (_started.exchange(true)) return;

    esp_netif_init();
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_LOGW(TAG, "event loop: %d", (int)e);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, password, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_set_ps(WIFI_PS_NONE);  // laagste latentie voor de audiostream
    esp_wifi_start();
}

bool wifiConnected()
{
    return _connected;
}

}  // namespace jarvis
