#include "ha.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "net.h"
#include "rcstage/stage.h"

namespace ha {

static const char* TAG = "ha";
static constexpr int kPollMs = 10000;

static std::mutex sLock;
static std::string sUrl, sToken, sEntities;    // sEntities: space-separated entity ids
static std::string sStatus = "not set up";
static std::map<std::string, std::string> sPublished;   // what the documents have now
static TaskHandle_t sTask;
static bool sPollAsked = true;                  // sLock: poll on the next wake
// Set points asked for, by entity then attribute (sLock), and who to tell when they're sent.
static std::map<std::string, std::map<std::string, float>> sSets;
static void (*sSetDone)(const std::string&, const std::string&);

static std::string load(nvs_handle_t h, const char* key) {
    size_t n = 0;
    if (nvs_get_str(h, key, nullptr, &n) != ESP_OK || n == 0) return {};
    std::string s(n, '\0');
    nvs_get_str(h, key, s.data(), &n);
    s.resize(n - 1);
    return s;
}

static bool save(const char* key, const std::string& value) {
    nvs_handle_t h;
    if (nvs_open("ha", NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, key, value.c_str()) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// Only changes reach the document, so an e-paper panel redraws when a value moves. `always`:
// after a call, when the document may have had it from a push since.
static void publish(const std::string& name, const std::string& value, bool always = false) {
    auto it = sPublished.find(name);
    if (!always && it != sPublished.end() && it->second == value) return;
    sPublished[name] = value;
    rcstage::setNamed(name, value);
}

static std::string scalar(const cJSON* v) {
    char b[32];
    if (cJSON_IsString(v)) return v->valuestring;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? "1" : "0";
    if (cJSON_IsNumber(v)) { snprintf(b, sizeof b, "%g", v->valuedouble); return b; }
    return {};   // null
}

// GET <url>/api/states/<id>; publishes the state and its scalar attributes.
static bool pollEntity(const std::string& url, const std::string& token, const std::string& id,
                       std::string& err, bool always = false) {
    std::string body;
    esp_http_client_config_t cfg = {};
    const std::string full = url + "/api/states/" + id;
    cfg.url = full.c_str();
    cfg.timeout_ms = 5000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    const std::string auth = "Bearer " + token;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
    bool ok = false;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        err = "can't reach " + url;
    } else {
        esp_http_client_fetch_headers(c);
        const int code = esp_http_client_get_status_code(c);
        char buf[512];
        int n;
        while ((n = esp_http_client_read(c, buf, sizeof buf)) > 0) body.append(buf, n);
        if (code == 200) ok = true;
        else err = id + ": HTTP " + std::to_string(code);
    }
    esp_http_client_cleanup(c);
    if (!ok) return false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (!root) { err = id + ": bad JSON"; return false; }
    publish("ha." + id, scalar(cJSON_GetObjectItem(root, "state")), always);
    const cJSON* attrs = cJSON_GetObjectItem(root, "attributes");
    const cJSON* a;
    cJSON_ArrayForEach(a, attrs) {
        if (cJSON_IsArray(a) || cJSON_IsObject(a)) continue;
        publish("ha." + id + "." + a->string, scalar(a), always);
    }
    cJSON_Delete(root);
    return true;
}

// POST <url>/api/services/climate/set_temperature {"entity_id": id, attr: value, ...}.
static bool setTemperature(const std::string& url, const std::string& token, const std::string& id,
                           const std::map<std::string, float>& attrs, std::string& err) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "entity_id", id.c_str());
    for (auto& [k, v] : attrs) cJSON_AddNumberToObject(body, k.c_str(), v);
    char* json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    esp_http_client_config_t cfg = {};
    const std::string full = url + "/api/services/climate/set_temperature";
    cfg.url = full.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 5000;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    const std::string auth = "Bearer " + token;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, strlen(json));
    bool ok = false;
    if (esp_http_client_perform(c) != ESP_OK) {
        err = "can't reach " + url;
    } else {
        const int code = esp_http_client_get_status_code(c);
        if (code == 200) ok = true;
        else err = id + ": HTTP " + std::to_string(code);
    }
    esp_http_client_cleanup(c);
    ESP_LOGI(TAG, "set_temperature %s %s: %s", id.c_str(), json, ok ? "ok" : err.c_str());
    cJSON_free(json);
    return ok;
}

// Send what was asked (merged per entity), read each entity back, tell whoever asked.
static void sendSets(const std::string& url, const std::string& token) {
    std::map<std::string, std::map<std::string, float>> sets;
    {
        std::lock_guard<std::mutex> g(sLock);
        sets.swap(sSets);
    }
    for (auto& [id, attrs] : sets) {
        std::string err;
        if (url.empty() || token.empty()) ESP_LOGW(TAG, "set %s: not set up (ha url / ha token)", id.c_str());
        else if (setTemperature(url, token, id, attrs, err)) pollEntity(url, token, id, err, true);
        else ESP_LOGW(TAG, "set %s: %s", id.c_str(), err.c_str());
        if (sSetDone)
            for (auto& [k, v] : attrs) sSetDone(id, k);
    }
}

static void task(void*) {
    while (true) {
        std::string url, token, entities;
        bool poll, sets;
        {
            std::lock_guard<std::mutex> g(sLock);
            url = sUrl, token = sToken, entities = sEntities;
            poll = sPollAsked, sets = !sSets.empty();
            sPollAsked = false;
        }
        if (sets) {
            vTaskDelay(pdMS_TO_TICKS(150));   // more taps on the way: one call for them
            sendSets(url, token);
        }
        if (!poll) {
        } else if (url.empty() || token.empty()) {
            std::lock_guard<std::mutex> g(sLock);
            sStatus = "not set up: ha url / ha token (and ha entities to poll)";
        } else if (entities.empty()) {
            std::lock_guard<std::mutex> g(sLock);
            sStatus = "calls only: no entities to poll (Home Assistant pushes them)";
        } else if (!net::connected()) {
            std::lock_guard<std::mutex> g(sLock);
            sStatus = "waiting for Wi-Fi";
            publish("ha.online", "0");
        } else {
            std::istringstream in(entities);
            std::string id, err;
            int good = 0, all = 0;
            while (in >> id) {
                all++;
                if (pollEntity(url, token, id, err)) good++;
            }
            if (good < all) ESP_LOGW(TAG, "%d of %d entities: %s", all - good, all, err.c_str());
            publish("ha.online", good ? "1" : "0");
            std::lock_guard<std::mutex> g(sLock);
            sStatus = good == all ? "online, " + std::to_string(all) + " entities"
                                  : std::to_string(good) + " of " + std::to_string(all) + " entities: " + err;
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs)) == 0) {
            std::lock_guard<std::mutex> g(sLock);
            sPollAsked = true;   // the poll's time
        }
    }
}

void start() {
    nvs_handle_t h;
    if (nvs_open("ha", NVS_READONLY, &h) == ESP_OK) {
        sUrl = load(h, "url"), sToken = load(h, "token"), sEntities = load(h, "entities");
        nvs_close(h);
    }
    xTaskCreatePinnedToCore(task, "ha", 6144, nullptr, 2, &sTask, 0);
}

void pollNow() {
    {
        std::lock_guard<std::mutex> g(sLock);
        sPollAsked = true;
    }
    if (sTask) xTaskNotifyGive(sTask);
}

void setClimate(const std::string& entity, const std::string& attr, float value) {
    {
        std::lock_guard<std::mutex> g(sLock);
        sSets[entity][attr] = value;
    }
    if (sTask) xTaskNotifyGive(sTask);
}

void onSetDone(void (*done)(const std::string& entity, const std::string& attr)) { sSetDone = done; }

// ha | ha url URL | ha token TOKEN | ha entities ID ID ... | ha poll
bool console(const std::string& args) {
    size_t sp = args.find(' ');
    const std::string verb = args.substr(0, sp), rest = sp == std::string::npos ? "" : args.substr(sp + 1);
    std::lock_guard<std::mutex> g(sLock);
    if (verb.empty()) {
        printf("ha: %s\n  url %s\n  token %s\n  entities %s\n", sStatus.c_str(),
               sUrl.empty() ? "-" : sUrl.c_str(), sToken.empty() ? "-" : "(saved)",
               sEntities.empty() ? "-" : sEntities.c_str());
        return true;
    }
    if (verb == "poll") { sPollAsked = true; if (sTask) xTaskNotifyGive(sTask); return true; }
    const char* key = verb == "url" ? "url" : verb == "token" ? "token" : verb == "entities" ? "entities" : nullptr;
    if (!key) return false;
    std::string value = rest;
    if (verb == "url") while (!value.empty() && value.back() == '/') value.pop_back();
    if (!save(key, value)) { printf("ha: save failed\n"); return true; }
    (verb == "url" ? sUrl : verb == "token" ? sToken : sEntities) = value;
    if (verb == "token") printf("ha: token saved (%u characters)\n", (unsigned) value.size());
    else printf("ha: %s saved\n", key);
    sPollAsked = true;   // sLock is held: not pollNow()
    if (sTask) xTaskNotifyGive(sTask);
    return true;
}

}  // namespace ha
