#include "ha_ctl.h"
#include "wifi_sta.h"
#include <ArduinoJson.h>
#include <atomic>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mqtt_client.h>
#include <nvs.h>
#include <mqtt_secrets.h>
#include <secrets.h>
#include <string>

static const char* TAG = "JCTL";

namespace jarvis::ctl {

static std::atomic<float> s_intensity[7] = {{50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}};   // per vorm: 0 Audio, 1 Sterrenhemel, 2 (Water, weg), 3 Aarde, 4 Maan, 5 Mars, 6 Jupiter
static std::atomic<float> s_sens[7]      = {{50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}, {50.0f}};
static constexpr int kModeCount = 7;
static std::atomic<int> s_mode{1};   // 0 Audio (golf), 1 Sterrenhemel, 2 niet meer gebruikt (was Water), 3 Aarde, 4 Maan, 5 Mars, 6 Jupiter
// Sterrenhemel vervangt Cirkel en Water (Man on the moon-thema, 30-10); nummers van de bollen blijven gelijk
static const char* const kModes[] = {"Audio", "Sterrenhemel", "-", "Aarde", "Maan", "Mars", "Jupiter"};
// tijdelijk alleen de bollen (Aarde, Maan, Mars, Jupiter) (besluit Mathijs 28-10); de draden-modi staan in tag backup/draden-modi
static std::atomic<bool> s_mic{true};
static std::atomic<bool> s_klok{true};
static std::atomic<bool> s_slaap{true};
static std::atomic<int> s_vol{60};
static std::atomic<int> s_hel{80};
static std::atomic<bool> s_nacht{true};   // nachtklok: rode maan met klok, gedimd
static std::atomic<int> s_nvan{22}, s_ntot{7};   // van/tot (uur)
static std::atomic<bool> s_nachtNu{false};       // nachtstand met de hand: ook buiten van/tot
static std::atomic<int> s_melk{50};              // hoe sterk de melkweg op de muziek reageert (0..100)
static std::atomic<bool> s_slaapNu{false};
static std::atomic<bool> s_ok{false};

// Leest de status van een entiteit; geeft de "state"-tekst of "" bij een fout.
static std::string readState(const char* entity)
{
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%d/api/states/%s", HA_HOST, HA_PORT, entity);
    esp_http_client_config_t cfg = {};
    cfg.url                      = url;
    cfg.timeout_ms               = 3000;
    esp_http_client_handle_t c   = esp_http_client_init(&cfg);
    if (!c) return "";
    esp_http_client_set_header(c, "Authorization", "Bearer " HA_TOKEN);
    std::string out;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            char buf[512];
            std::string body;
            int n;
            while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0) body.append(buf, n);
            JsonDocument doc;
            JsonDocument filter;
            filter["state"] = true;
            if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) out = (const char*)(doc["state"] | "");
        }
    }
    esp_http_client_cleanup(c);
    return out;
}

// ---------------------------------------------------------------- opslaan: standen overleven een herstart
static void saveState()
{
    nvs_handle_t h;
    if (nvs_open("jarvisctl", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "heft", (uint8_t)(s_intensity[0].load() + 0.5f));
    nvs_set_u8(h, "mic", s_mic.load() ? 1 : 0);
    nvs_set_u8(h, "klok", s_klok.load() ? 1 : 0);
    nvs_set_u8(h, "slaap", s_slaap.load() ? 1 : 0);
    nvs_set_u8(h, "vol", (uint8_t)s_vol.load());
    nvs_set_u8(h, "hel", (uint8_t)s_hel.load());
    nvs_set_u8(h, "gev", (uint8_t)(s_sens[0].load() + 0.5f));
    nvs_set_u8(h, "heftc", (uint8_t)(s_intensity[1].load() + 0.5f));
    nvs_set_u8(h, "gevc", (uint8_t)(s_sens[1].load() + 0.5f));
    nvs_set_u8(h, "heftw", (uint8_t)(s_intensity[2].load() + 0.5f));
    nvs_set_u8(h, "gevw", (uint8_t)(s_sens[2].load() + 0.5f));
    nvs_set_u8(h, "hefta", (uint8_t)(s_intensity[3].load() + 0.5f));
    nvs_set_u8(h, "geva", (uint8_t)(s_sens[3].load() + 0.5f));
    nvs_set_u8(h, "mode", (uint8_t)s_mode.load());
    nvs_set_u8(h, "nacht", s_nacht.load() ? 1 : 0);
    nvs_set_u8(h, "nvan", (uint8_t)s_nvan.load());
    nvs_set_u8(h, "ntot", (uint8_t)s_ntot.load());
    nvs_set_u8(h, "nnu", s_nachtNu.load() ? 1 : 0);
    nvs_set_u8(h, "melk", (uint8_t)s_melk.load());
    nvs_commit(h);
    nvs_close(h);
}

static void loadState()
{
    nvs_handle_t h;
    if (nvs_open("jarvisctl", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t v;
    if (nvs_get_u8(h, "heft", &v) == ESP_OK) s_intensity[0] = v;
    if (nvs_get_u8(h, "mic", &v) == ESP_OK) s_mic = v != 0;
    if (nvs_get_u8(h, "klok", &v) == ESP_OK) s_klok = v != 0;
    if (nvs_get_u8(h, "slaap", &v) == ESP_OK) s_slaap = v != 0;
    if (nvs_get_u8(h, "vol", &v) == ESP_OK) s_vol = v;
    if (nvs_get_u8(h, "hel", &v) == ESP_OK) s_hel = v;
    if (nvs_get_u8(h, "gev", &v) == ESP_OK) s_sens[0] = v;
    if (nvs_get_u8(h, "heftc", &v) == ESP_OK) s_intensity[1] = v;
    if (nvs_get_u8(h, "gevc", &v) == ESP_OK) s_sens[1] = v;
    if (nvs_get_u8(h, "heftw", &v) == ESP_OK) s_intensity[2] = v;
    if (nvs_get_u8(h, "gevw", &v) == ESP_OK) s_sens[2] = v;
    if (nvs_get_u8(h, "hefta", &v) == ESP_OK) s_intensity[3] = v;
    if (nvs_get_u8(h, "geva", &v) == ESP_OK) s_sens[3] = v;
    if (nvs_get_u8(h, "mode", &v) == ESP_OK && v < kModeCount) s_mode = v == 2 ? 1 : v;   // Water bestaat niet meer
    if (nvs_get_u8(h, "nacht", &v) == ESP_OK) s_nacht = v != 0;
    if (nvs_get_u8(h, "nvan", &v) == ESP_OK && v < 24) s_nvan = v;
    if (nvs_get_u8(h, "ntot", &v) == ESP_OK && v < 24) s_ntot = v;
    if (nvs_get_u8(h, "nnu", &v) == ESP_OK) s_nachtNu = v != 0;
    if (nvs_get_u8(h, "melk", &v) == ESP_OK && v <= 100) s_melk = v;
    nvs_close(h);
}

// ---------------------------------------------------------------- MQTT (apparaat "Stopwatch" via discovery)
static esp_mqtt_client_handle_t s_mqtt = nullptr;
static const char* kBase = "stopwatch";

static void pub(const char* topic, const char* payload, bool retain)
{
    if (s_mqtt) esp_mqtt_client_publish(s_mqtt, topic, payload, 0, 1, retain ? 1 : 0);
}

static void publishDiscovery()
{
    const char* dev =
        "\"dev\":{\"ids\":[\"stopwatch_jarvis\"],\"name\":\"Stopwatch\",\"mf\":\"M5Stack\",\"mdl\":\"StopWatch\"},"
        "\"avty_t\":\"stopwatch/status\"";
    char b[640];
    // per vorm eigen schuifjes: Golf houdt de oude entiteiten (zelfde id), Sterrenhemel die van Cirkel
    static const char* const nm[4]  = {"Audio", "Sterrenhemel", "Water", "Aarde"};
    static const char* const sfx[4] = {"", "_cirkel", "_water", "_aarde"};
    for (int m = 0; m < 4; m++) {
        if (m >= 2) {   // Water is weg en de bollen reageren niet op geluid: die schuifjes uit HA halen
            char t[80];
            snprintf(t, sizeof(t), "homeassistant/number/stopwatch_heftigheid%s/config", sfx[m]);
            pub(t, "", true);
            snprintf(t, sizeof(t), "homeassistant/number/stopwatch_gevoeligheid%s/config", sfx[m]);
            pub(t, "", true);
            continue;
        }
        snprintf(b, sizeof(b),
                 "{\"name\":\"%s heftigheid\",\"uniq_id\":\"stopwatch_heftigheid%s\",\"cmd_t\":\"stopwatch/heftigheid%s/set\","
                 "\"stat_t\":\"stopwatch/heftigheid%s\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:sine-wave\",\"ret\":true,%s}",
                 nm[m], sfx[m], sfx[m], sfx[m], dev);
        char t[80];
        snprintf(t, sizeof(t), "homeassistant/number/stopwatch_heftigheid%s/config", sfx[m]);
        pub(t, b, true);
        snprintf(b, sizeof(b),
                 "{\"name\":\"%s gevoeligheid\",\"uniq_id\":\"stopwatch_gevoeligheid%s\",\"cmd_t\":\"stopwatch/gevoeligheid%s/set\","
                 "\"stat_t\":\"stopwatch/gevoeligheid%s\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:ear-hearing\",\"ret\":true,%s}",
                 nm[m], sfx[m], sfx[m], sfx[m], dev);
        snprintf(t, sizeof(t), "homeassistant/number/stopwatch_gevoeligheid%s/config", sfx[m]);
        pub(t, b, true);
    }
    snprintf(b, sizeof(b),
             "{\"name\":\"Microfoon\",\"uniq_id\":\"stopwatch_microfoon\",\"cmd_t\":\"stopwatch/microfoon/set\","
             "\"stat_t\":\"stopwatch/microfoon\",\"icon\":\"mdi:microphone\",\"ret\":true,%s}",
             dev);
    pub("homeassistant/switch/stopwatch_microfoon/config", b, true);
    snprintf(b, sizeof(b),
             "{\"name\":\"Klok\",\"uniq_id\":\"stopwatch_klok\",\"cmd_t\":\"stopwatch/klok/set\","
             "\"stat_t\":\"stopwatch/klok\",\"icon\":\"mdi:clock-outline\",\"ret\":true,%s}",
             dev);
    pub("homeassistant/switch/stopwatch_klok/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Automatisch slapen\",\"uniq_id\":\"stopwatch_slaap\",\"cmd_t\":\"stopwatch/slaap/set\",\"stat_t\":\"stopwatch/slaap\",\"icon\":\"mdi:sleep\",\"ret\":true,%s}", dev);
    pub("homeassistant/switch/stopwatch_slaap/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Volume\",\"uniq_id\":\"stopwatch_volume\",\"cmd_t\":\"stopwatch/volume/set\",\"stat_t\":\"stopwatch/volume\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:volume-high\",\"ret\":true,%s}", dev);
    pub("homeassistant/number/stopwatch_volume/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Helderheid\",\"uniq_id\":\"stopwatch_helderheid\",\"cmd_t\":\"stopwatch/helderheid/set\",\"stat_t\":\"stopwatch/helderheid\",\"min\":5,\"max\":100,\"step\":1,\"mode\":\"slider\",\"unit_of_meas\":\"%%\",\"icon\":\"mdi:brightness-6\",\"ret\":true,%s}", dev);
    pub("homeassistant/number/stopwatch_helderheid/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Slapen\",\"uniq_id\":\"stopwatch_slapen\",\"cmd_t\":\"stopwatch/slapen/set\",\"icon\":\"mdi:power-sleep\",%s}", dev);
    pub("homeassistant/button/stopwatch_slapen/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Nachtklok\",\"uniq_id\":\"stopwatch_nacht\",\"cmd_t\":\"stopwatch/nacht/set\",\"stat_t\":\"stopwatch/nacht\",\"icon\":\"mdi:weather-night\",\"ret\":true,%s}", dev);
    pub("homeassistant/switch/stopwatch_nacht/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Nachtklok van\",\"uniq_id\":\"stopwatch_nachtvan\",\"cmd_t\":\"stopwatch/nachtvan/set\",\"stat_t\":\"stopwatch/nachtvan\",\"min\":0,\"max\":23,\"step\":1,\"mode\":\"box\",\"unit_of_meas\":\"h\",\"icon\":\"mdi:clock-start\",\"ret\":true,%s}", dev);
    pub("homeassistant/number/stopwatch_nachtvan/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Nachtklok tot\",\"uniq_id\":\"stopwatch_nachttot\",\"cmd_t\":\"stopwatch/nachttot/set\",\"stat_t\":\"stopwatch/nachttot\",\"min\":0,\"max\":23,\"step\":1,\"mode\":\"box\",\"unit_of_meas\":\"h\",\"icon\":\"mdi:clock-end\",\"ret\":true,%s}", dev);
    pub("homeassistant/number/stopwatch_nachttot/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Nachtstand\",\"uniq_id\":\"stopwatch_nachtnu\",\"cmd_t\":\"stopwatch/nachtnu/set\",\"stat_t\":\"stopwatch/nachtnu\",\"icon\":\"mdi:power-sleep\",\"ret\":true,%s}", dev);
    pub("homeassistant/switch/stopwatch_nachtnu/config", b, true);
    snprintf(b, sizeof(b), "{\"name\":\"Melkweg\",\"uniq_id\":\"stopwatch_melkweg\",\"cmd_t\":\"stopwatch/melkweg/set\",\"stat_t\":\"stopwatch/melkweg\",\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\",\"icon\":\"mdi:creation\",\"ret\":true,%s}", dev);
    pub("homeassistant/number/stopwatch_melkweg/config", b, true);
    pub("homeassistant/number/stopwatch_3d/config", "", true);   // oude 3D-schuif uit HA halen
    snprintf(b, sizeof(b),
             "{\"name\":\"Modus\",\"uniq_id\":\"stopwatch_modus\",\"cmd_t\":\"stopwatch/modus/set\","
             "\"stat_t\":\"stopwatch/modus\",%s,\"icon\":\"mdi:shape\",\"ret\":true,%s}",
             "\"options\":[\"Audio\",\"Sterrenhemel\",\"Aarde\",\"Maan\",\"Mars\",\"Jupiter\"]", dev);
    pub("homeassistant/select/stopwatch_modus/config", b, true);
}

static void publishState()
{
    char v[16];
    snprintf(v, sizeof(v), "%d", (int)(s_intensity[0].load() + 0.5f));
    pub("stopwatch/heftigheid", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_intensity[1].load() + 0.5f));
    pub("stopwatch/heftigheid_cirkel", v, true);
    pub("stopwatch/microfoon", s_mic.load() ? "ON" : "OFF", true);
    pub("stopwatch/klok", s_klok.load() ? "ON" : "OFF", true);
    pub("stopwatch/slaap", s_slaap.load() ? "ON" : "OFF", true);
    snprintf(v, sizeof(v), "%d", s_vol.load());
    pub("stopwatch/volume", v, true);
    snprintf(v, sizeof(v), "%d", s_hel.load());
    pub("stopwatch/helderheid", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_sens[0].load() + 0.5f));
    pub("stopwatch/gevoeligheid", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_sens[1].load() + 0.5f));
    pub("stopwatch/gevoeligheid_cirkel", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_intensity[2].load() + 0.5f));
    pub("stopwatch/heftigheid_water", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_sens[2].load() + 0.5f));
    pub("stopwatch/gevoeligheid_water", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_intensity[3].load() + 0.5f));
    pub("stopwatch/heftigheid_aarde", v, true);
    snprintf(v, sizeof(v), "%d", (int)(s_sens[3].load() + 0.5f));
    pub("stopwatch/gevoeligheid_aarde", v, true);
    pub("stopwatch/modus", kModes[mode()], true);
    pub("stopwatch/nacht", s_nacht.load() ? "ON" : "OFF", true);
    snprintf(v, sizeof(v), "%d", s_nvan.load());
    pub("stopwatch/nachtvan", v, true);
    snprintf(v, sizeof(v), "%d", s_ntot.load());
    pub("stopwatch/nachttot", v, true);
    pub("stopwatch/nachtnu", s_nachtNu.load() ? "ON" : "OFF", true);
    snprintf(v, sizeof(v), "%d", s_melk.load());
    pub("stopwatch/melkweg", v, true);
}

static void mqttEvent(void*, esp_event_base_t, int32_t id, void* data)
{
    auto* e = (esp_mqtt_event_handle_t)data;
    if (id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT verbonden");
        s_ok = true;
        pub("stopwatch/status", "online", true);
        publishDiscovery();
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/heftigheid/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/microfoon/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/klok/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/slaap/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/volume/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/helderheid/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/slapen/set", 0);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/gevoeligheid/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/heftigheid_cirkel/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/gevoeligheid_cirkel/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/heftigheid_water/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/gevoeligheid_water/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/heftigheid_aarde/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/gevoeligheid_aarde/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/modus/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/nacht/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/nachtvan/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/nachttot/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/nachtnu/set", 1);
        esp_mqtt_client_subscribe(s_mqtt, "stopwatch/melkweg/set", 1);
        publishState();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_ok = false;
    } else if (id == MQTT_EVENT_DATA) {
        std::string topic(e->topic, e->topic_len), val(e->data, e->data_len);
        float f = atof(val.c_str());
        f = f < 0 ? 0 : (f > 100 ? 100 : f);
        if (topic == "stopwatch/heftigheid/set") {
            s_intensity[0] = f;
        } else if (topic == "stopwatch/heftigheid_cirkel/set") {
            s_intensity[1] = f;
        } else if (topic == "stopwatch/gevoeligheid/set") {
            s_sens[0] = f;
        } else if (topic == "stopwatch/gevoeligheid_cirkel/set") {
            s_sens[1] = f;
        } else if (topic == "stopwatch/heftigheid_water/set") {
            s_intensity[2] = f;
        } else if (topic == "stopwatch/gevoeligheid_water/set") {
            s_sens[2] = f;
        } else if (topic == "stopwatch/heftigheid_aarde/set") {
            s_intensity[3] = f;
        } else if (topic == "stopwatch/gevoeligheid_aarde/set") {
            s_sens[3] = f;
        } else if (topic == "stopwatch/modus/set") {
            for (int i = 0; i < kModeCount; i++) if (val == kModes[i]) s_mode = i;
            if (val == "Golf") s_mode = 0;   // oude naam van Audio
            if (val == "Cirkel" || val == "Water") s_mode = 1;   // oude namen
        } else if (topic == "stopwatch/microfoon/set") {
            s_mic = (val == "ON" || val == "on" || val == "1");
        } else if (topic == "stopwatch/klok/set") {
            s_klok = (val == "ON" || val == "on" || val == "1");
        } else if (topic == "stopwatch/volume/set") {
            s_vol = (int)(f + 0.5f);
        } else if (topic == "stopwatch/helderheid/set") {
            s_hel = f < 5 ? 5 : (f > 100 ? 100 : (int)(f + 0.5f));
        } else if (topic == "stopwatch/nacht/set") {
            s_nacht = (val == "ON" || val == "on" || val == "1");
        } else if (topic == "stopwatch/nachtvan/set") {
            s_nvan = (int)(f + 0.5f) % 24;
        } else if (topic == "stopwatch/nachttot/set") {
            s_ntot = (int)(f + 0.5f) % 24;
        } else if (topic == "stopwatch/nachtnu/set") {
            s_nachtNu = (val == "ON" || val == "on" || val == "1");
        } else if (topic == "stopwatch/melkweg/set") {
            s_melk = (int)(f + 0.5f);
        } else if (topic == "stopwatch/slaap/set") {
            s_slaap = (val == "ON" || val == "on" || val == "1");
        } else if (topic == "stopwatch/slapen/set") {
            if (e->retain) {   // bewaard bericht: niet slapen (anders meteen weer na elk opstarten), en opruimen
                if (e->data_len) esp_mqtt_client_publish(s_mqtt, "stopwatch/slapen/set", "", 0, 1, 1);
                return;
            }
            s_slaapNu = true;
            return;
        }
        saveState();
        publishState();
    }
}

static void startMqtt()
{
    esp_mqtt_client_config_t cfg = {};
    static char uri[96];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
    cfg.broker.address.uri                  = uri;
    cfg.credentials.username                = MQTT_USER;
    cfg.credentials.authentication.password = MQTT_PASS;
    cfg.credentials.client_id               = "stopwatch-jarvis";
    cfg.session.last_will.topic             = "stopwatch/status";
    cfg.session.last_will.msg               = "offline";
    cfg.session.last_will.retain            = 1;
    cfg.session.last_will.qos               = 1;
    s_mqtt = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_mqtt, MQTT_EVENT_ANY, mqttEvent, nullptr);
    esp_mqtt_client_start(s_mqtt);
}

static void task(void*)
{
    for (;;) {
        if (wifiConnected()) {
            std::string a = readState("input_number.stopwatch_heftigheid");
            std::string b = readState("input_boolean.stopwatch_microfoon");
            if (!a.empty()) s_intensity[0] = atof(a.c_str());
            if (!b.empty()) s_mic = (b == "on");
            bool ok = !a.empty() && !b.empty();
            if (ok != s_ok.load()) ESP_LOGI(TAG, "HA-bediening %s", ok ? "verbonden" : "niet bereikbaar");
            s_ok = ok;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void begin()
{
    loadState();
    wifiStart(WIFI_SSID, WIFI_PASSWORD);
    if (MQTT_USER[0] != 0) {  // MQTT ingevuld: apparaat "Stopwatch" via discovery
        xTaskCreate([](void*) {
            while (!wifiConnected()) vTaskDelay(pdMS_TO_TICKS(500));
            startMqtt();
            vTaskDelete(nullptr);
        }, "jarvis_mqtt", 4 * 1024, nullptr, 1, nullptr);
        return;
    }
    xTaskCreate(task, "jarvis_ctl", 6 * 1024, nullptr, 1, nullptr);  // zonder MQTT: HA-helpers via REST
}

int mode()
{
    return s_mode.load();
}
float intensity() { return s_intensity[mode()].load(); }   // van de gekozen vorm
float sensitivity() { return s_sens[mode()].load(); }
// knop A: planeten (aarde, maan, Mars, Jupiter), knop B: Audio en Sterrenhemel;
// wissel je van groep, dan kom je terug waar je in die groep was
static int s_planeet = 3, s_audio = 0;
void cycleMode()
{
    if (mode() < 3) { s_audio = mode(); s_mode = s_planeet; }
    else s_mode = mode() >= kModeCount - 1 ? 3 : mode() + 1;
    saveState();
    publishState();
}
void audioMode()
{
    if (mode() >= 3) { s_planeet = mode(); s_mode = s_audio; }
    else s_mode = mode() == 0 ? 1 : 0;
    saveState();
    publishState();
}
bool micOn() { return s_mic.load(); }
bool klokOn() { return s_klok.load(); }
int volume() { return s_vol.load(); }
int helderheid() { return s_hel.load(); }
bool autoSleep() { return s_slaap.load(); }
bool nachtklok() { return s_nacht.load(); }
bool nachtNu() { return s_nachtNu.load(); }
int melkweg() { return s_melk.load(); }
int nachtVan() { return s_nvan.load(); }
int nachtTot() { return s_ntot.load(); }
bool takeSleepNow() { return s_slaapNu.exchange(false); }
void goingToSleep()
{
    if (!s_mqtt || !s_ok.load()) return;
    esp_mqtt_client_publish(s_mqtt, "stopwatch/status", "offline", 0, 1, true);
    vTaskDelay(pdMS_TO_TICKS(300));
}
bool connected() { return s_ok.load(); }

}  // namespace jarvis::ctl
