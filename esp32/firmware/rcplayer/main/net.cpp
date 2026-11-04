#include "net.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "board/board.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_MCU
#include "esp_hosted.h"
#endif
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "rcstage/stage.h"
#include "sdkconfig.h"

#include "kobento_net.hpp"   // the protocol (bodies, parse); header-only, from kobento-net

static const char* TAG = "net";

namespace net {

static esp_netif_t* sNetif = nullptr;
static httpd_handle_t sHttp = nullptr;
static bool sStarted = false, sConnected = false;
static char sIp[16] = "";
static std::mutex sLock;
static std::deque<Received> sInbox;

static constexpr size_t kMaxDoc = 4 * 1024 * 1024;

// ── networks ─────────────────────────────────────────────────────────────────────────────
//
// Saved networks live in NVS ("wifi"/"nets": up to kMaxSaved of [len ssid len pass]; an empty
// password is an open network), with the last one joined in "wifi"/"last". A task owns the
// radio. At start it scans and joins, in order: the last network if it is in range, the
// strongest saved network with a password, the strongest saved open one. A network that
// fails kTries times in a row is set aside and the next one is tried; with nothing to join
// it scans again every kRescanMs.

static constexpr int kMaxSaved = 8, kTries = 4, kRescanMs = 30000;

struct Saved { std::string ssid, pass; };
struct Seen { std::string ssid; int rssi; bool secured; };

enum class State { Idle, Connecting, Connected, Failed, NoNetwork };
enum class Op { Auto, Join, Scan, Disconnected, GotIp };
struct Msg { Op op; std::string ssid; int reason = 0; bool print = false; };

static std::mutex sNetLock;                // guards the state below, shared between tasks
static std::vector<Saved> sSaved;
static std::vector<Seen> sSeen;
static std::string sLast, sTarget, sFailed, sNotice;
static State sState = State::Idle;
static bool sScanning = false;
static std::deque<Msg> sMsgs;
static TaskHandle_t sTask;
static SemaphoreHandle_t sLeft;            // given when a disconnect we asked for lands
static volatile bool sAwaitLeave = false;
static void (*sOnChange)() = nullptr;

static void changed() { if (sOnChange) sOnChange(); }

static void post(Msg m) {
    {
        std::lock_guard<std::mutex> g(sNetLock);
        sMsgs.push_back(std::move(m));
    }
    if (sTask) xTaskNotifyGive(sTask);
}

static void saveAll() {   // sNetLock held
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    std::string blob;
    for (auto& n : sSaved) {
        blob += (char) n.ssid.size(); blob += n.ssid;
        blob += (char) n.pass.size(); blob += n.pass;
    }
    nvs_set_blob(h, "nets", blob.data(), blob.size());
    nvs_set_str(h, "last", sLast.c_str());
    nvs_commit(h);
    nvs_close(h);
}

static void loadAll() {
    std::lock_guard<std::mutex> g(sNetLock);
    sSaved.clear();
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
        size_t n = 0;
        if (nvs_get_blob(h, "nets", nullptr, &n) == ESP_OK && n) {
            std::string blob(n, 0);
            nvs_get_blob(h, "nets", blob.data(), &n);
            for (size_t i = 0; i < n;) {
                Saved s;
                size_t l = (uint8_t) blob[i++];
                s.ssid = blob.substr(i, l); i += l;
                if (i >= n) break;
                l = (uint8_t) blob[i++];
                s.pass = blob.substr(i, l); i += l;
                if (!s.ssid.empty()) sSaved.push_back(s);
            }
        }
        char buf[33] = "";
        size_t l = sizeof(buf);
        if (nvs_get_str(h, "last", buf, &l) == ESP_OK) sLast = buf;
        // Earlier firmware kept a single network ("ssid"/"pass"); it moves into the list.
        char s[33] = "", p[65] = "";
        size_t ls = sizeof(s), lp = sizeof(p);
        if (nvs_get_str(h, "ssid", s, &ls) == ESP_OK && s[0]) {
            nvs_get_str(h, "pass", p, &lp);
            nvs_erase_key(h, "ssid");
            nvs_erase_key(h, "pass");
            nvs_close(h);
            sSaved.push_back({s, p});
            if (sLast.empty()) sLast = s;
            saveAll();
            ESP_LOGI(TAG, "moved saved network '%s' into the list", s);
            return;
        }
        nvs_close(h);
    }
    if (sSaved.empty() && CONFIG_RCPLAYER_WIFI_SSID[0])
        sSaved.push_back({CONFIG_RCPLAYER_WIFI_SSID, CONFIG_RCPLAYER_WIFI_PASSWORD});
}

static const Saved* findSaved(const std::string& ssid) {   // sNetLock held
    for (auto& n : sSaved) if (n.ssid == ssid) return &n;
    return nullptr;
}
static const Seen* findSeen(const std::string& ssid) {     // sNetLock held
    for (auto& n : sSeen) if (n.ssid == ssid) return &n;
    return nullptr;
}

// ── the Wi-Fi task ──

static int sTries = 0;
static int64_t sRescanAt = 0;   // ms since boot; 0 = none

static void doScan(bool print) {
    {
        std::lock_guard<std::mutex> g(sNetLock);
        if (sState == State::Connecting) {   // the radio refuses to scan mid-join
            if (print) printf("scan: busy joining, try again\n");
            return;
        }
        sScanning = true;
    }
    changed();
    wifi_scan_config_t sc = {};
    std::vector<Seen> seen;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        std::vector<wifi_ap_record_t> recs(n);
        esp_wifi_scan_get_ap_records(&n, recs.data());
        for (int i = 0; i < n; i++) {
            const char* ssid = (const char*) recs[i].ssid;
            if (!ssid[0]) continue;
            if (print) printf("  %-32s ch%-3d %d dBm%s\n", ssid, recs[i].primary, recs[i].rssi,
                              recs[i].authmode == WIFI_AUTH_OPEN ? "  open" : "");
            bool dup = false;
            for (auto& s : seen) dup |= s.ssid == ssid;   // strongest comes first; keep it
            if (!dup) seen.push_back({ssid, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN});
        }
        if (print) printf("scan: %u APs\n", n);
    } else if (print) {
        printf("scan failed\n");
    }
    {
        std::lock_guard<std::mutex> g(sNetLock);
        sSeen = std::move(seen);
        sScanning = false;
    }
    changed();
}

static void connectTo(const std::string& ssid, const std::string& pass) {
    bool busy;
    {
        std::lock_guard<std::mutex> g(sNetLock);
        busy = sState == State::Connecting || sState == State::Connected;
    }
    if (busy) {   // leave the current network first, so its disconnect isn't counted as ours
        sAwaitLeave = true;
        esp_wifi_disconnect();
        xSemaphoreTake(sLeft, pdMS_TO_TICKS(1500));
        sAwaitLeave = false;
    }
    wifi_config_t wc = {};
    strncpy((char*) wc.sta.ssid, ssid.c_str(), sizeof(wc.sta.ssid) - 1);
    strncpy((char*) wc.sta.password, pass.c_str(), sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    {
        std::lock_guard<std::mutex> g(sNetLock);
        sTarget = ssid;
        sState = State::Connecting;
        sTries = 0;
        sRescanAt = 0;
    }
    sConnected = false;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
    ESP_LOGI(TAG, "connecting to '%s'%s", ssid.c_str(), pass.empty() ? " (open)" : "");
    changed();
}

// Scan, then join the best saved network in range other than `skip`.
static void autoJoin(const std::string& skip) {
    doScan(false);
    Saved pick;
    bool none;
    {
        std::lock_guard<std::mutex> g(sNetLock);
        int best = -1000;
        for (auto& n : sSaved) {
            const Seen* s = findSeen(n.ssid);
            if (!s || n.ssid == skip) continue;
            int score = s->rssi + (n.ssid == sLast ? 1000 : n.pass.empty() ? 0 : 200);
            if (score > best) { best = score; pick = n; }
        }
        // None in range: the last network may be hidden from scans; try it anyway.
        if (pick.ssid.empty() && skip.empty())
            if (const Saved* l = findSaved(sLast)) pick = *l;
        none = sSaved.empty();
        if (pick.ssid.empty()) {
            if (sState != State::Failed) sState = none ? State::Idle : State::NoNetwork;
            sRescanAt = esp_timer_get_time() / 1000 + kRescanMs;
        }
    }
    if (pick.ssid.empty()) {
        ESP_LOGW(TAG, "%s", none ? "no saved networks; send 'wifi add SSID PASSWORD'"
                                 : "no saved network in range");
        changed();
        return;
    }
    connectTo(pick.ssid, pick.pass);
}

static void handle(const Msg& m) {
    switch (m.op) {
        case Op::Auto: autoJoin(""); break;
        case Op::Scan: doScan(m.print); break;
        case Op::Join: {
            Saved n;
            bool ok = false;
            {
                std::lock_guard<std::mutex> g(sNetLock);
                sNotice.clear();
                if (const Saved* s = findSaved(m.ssid)) {
                    n = *s;
                    ok = true;
                } else if (const Seen* s = findSeen(m.ssid); s && !s->secured) {
                    n = {m.ssid, ""};
                    ok = true;
                } else {
                    sNotice = m.ssid + " needs a password (wifi add)";
                }
            }
            if (ok) connectTo(n.ssid, n.pass);
            else changed();
            break;
        }
        case Op::Disconnected: {
            std::string failed;
            bool retry = false;
            {
                std::lock_guard<std::mutex> g(sNetLock);
                if (sState == State::Connected) {          // lost the link: rejoin it
                    sState = State::Connecting;
                    sTries = 0;
                    retry = true;
                } else if (sState == State::Connecting) {
                    if (++sTries >= kTries) {
                        sState = State::Failed;
                        sFailed = failed = sTarget;
                    } else {
                        retry = true;
                    }
                }
            }
            if (!failed.empty()) {
                ESP_LOGW(TAG, "couldn't join '%s' (reason %d)", failed.c_str(), m.reason);
                changed();
                autoJoin(failed);
            } else if (retry) {
                esp_wifi_connect();
                changed();
            }
            break;
        }
        case Op::GotIp: {
            {
                std::lock_guard<std::mutex> g(sNetLock);
                sState = State::Connected;
                sFailed.clear();
                sNotice.clear();
                // An open network joined from the list is remembered too.
                if (!findSaved(sTarget)) {
                    if ((int) sSaved.size() >= kMaxSaved) sSaved.erase(sSaved.begin());
                    sSaved.push_back({sTarget, ""});
                }
                sLast = sTarget;
                saveAll();
            }
            changed();
            break;
        }
    }
}

static void wifiTask(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        while (true) {
            Msg m;
            {
                std::lock_guard<std::mutex> g(sNetLock);
                if (sMsgs.empty()) break;
                m = std::move(sMsgs.front());
                sMsgs.pop_front();
            }
            handle(m);
        }
        if (sRescanAt && esp_timer_get_time() / 1000 >= sRescanAt) {
            sRescanAt = 0;
            autoJoin("");
        }
    }
}

// ── public ──

bool addNetwork(const std::string& ssid, const std::string& pass) {
    if (ssid.empty() || ssid.size() > 32 || pass.size() > 63) return false;
    {
        std::lock_guard<std::mutex> g(sNetLock);
        for (auto it = sSaved.begin(); it != sSaved.end(); ++it)
            if (it->ssid == ssid) { sSaved.erase(it); break; }
        if ((int) sSaved.size() >= kMaxSaved) sSaved.erase(sSaved.begin());
        sSaved.push_back({ssid, pass});
        saveAll();
    }
    changed();
    return true;
}

bool forget(const std::string& ssid) {
    bool found = false;
    {
        std::lock_guard<std::mutex> g(sNetLock);
        for (auto it = sSaved.begin(); it != sSaved.end(); ++it)
            if (it->ssid == ssid) { sSaved.erase(it); found = true; break; }
        if (sLast == ssid) sLast.clear();
        if (found) saveAll();
    }
    changed();
    return found;
}

void join(const std::string& ssid) { post({Op::Join, ssid}); }

bool setCredentials(const std::string& ssid, const std::string& pass) {
    if (!addNetwork(ssid, pass)) return false;
    if (sStarted) join(ssid);
    return true;
}

void scan(bool print) { post({Op::Scan, "", 0, print}); }

void onChange(void (*cb)()) { sOnChange = cb; }

bool connected() { return sConnected; }

std::string status() {
    if (!sStarted) return "wifi: not started";
    std::lock_guard<std::mutex> g(sNetLock);
    std::string s;
    switch (sState) {
        case State::Connected:
            s = "wifi: connected to '" + sTarget + "' ip " + sIp + " kobento-net port " +
                std::to_string(CONFIG_RCPLAYER_KOBENTO_PORT);
            break;
        case State::Connecting: s = "wifi: connecting to '" + sTarget + "'"; break;
        case State::Failed: s = "wifi: couldn't join '" + sFailed + "'"; break;
        case State::NoNetwork: s = "wifi: no saved network in range"; break;
        case State::Idle: s = "wifi: no saved networks (use: wifi add SSID PASSWORD)"; break;
    }
    s += "\nsaved networks:";
    for (auto& n : sSaved)
        s += "\n  " + n.ssid + (n.pass.empty() ? "  (open)" : "") + (n.ssid == sLast ? "  (last)" : "");
    return s;
}

View view() {
    std::lock_guard<std::mutex> g(sNetLock);
    View v;
    v.scanning = sScanning;
    auto signal = [](int rssi) { return std::max(1, std::min(100, 2 * (rssi + 100))); };
    switch (sState) {
        case State::Connected: v.status = "Connected to " + sTarget; break;
        case State::Connecting: v.status = "Connecting to " + sTarget + "…"; break;
        case State::Failed: v.status = "Couldn't join " + sFailed; break;
        case State::NoNetwork: v.status = "No saved network in range"; break;
        case State::Idle: v.status = "No saved networks"; break;
    }
    if (!sNotice.empty()) v.status = sNotice;
    // The current network, then saved ones in range, open ones in range, saved ones out of
    // range, and last the rest in range: they need a password, added on the console.
    const bool live = sState == State::Connected || sState == State::Connecting;
    const int liveState = sState == State::Connected ? 1 : 2;
    std::vector<Entry> current, saved, open, away, locked;
    for (auto& n : sSaved) {
        const Seen* s = findSeen(n.ssid);
        Entry e{n.ssid, 3, s ? signal(s->rssi) : 0, !n.pass.empty()};
        if (live && n.ssid == sTarget) { e.state = liveState; current.push_back(e); }
        else if (n.ssid == sFailed) { e.state = 6; (s ? saved : away).push_back(e); }
        else if (s) saved.push_back(e);
        else { e.state = 5; away.push_back(e); }
    }
    for (auto& s : sSeen) {
        if (findSaved(s.ssid)) continue;
        if (s.secured) { locked.push_back({s.ssid, 7, signal(s.rssi), true}); continue; }
        Entry e{s.ssid, 4, signal(s.rssi), false};
        if (live && s.ssid == sTarget) { e.state = liveState; current.push_back(e); }
        else open.push_back(e);
    }
    auto bySignal = [](const Entry& a, const Entry& b) { return a.signal > b.signal; };
    std::stable_sort(saved.begin(), saved.end(), bySignal);
    std::stable_sort(open.begin(), open.end(), bySignal);
    for (auto* l : {&current, &saved, &open, &away, &locked}) v.entries.insert(v.entries.end(), l->begin(), l->end());
    return v;
}

bool pollReceived(Received& out) {
    std::lock_guard<std::mutex> g(sLock);
    if (sInbox.empty()) return false;
    out = std::move(sInbox.front());
    sInbox.pop_front();
    return true;
}

// ── kobento-net HTTP receiver ───────────────────────────────────────────────────────────

static esp_err_t onInfo(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, kobento_net::infoBody(CONFIG_RCPLAYER_DEVICE_NAME).c_str());
}

static esp_err_t onSend(httpd_req_t* req) {
    char name[96] = "unnamed";
    httpd_req_get_hdr_value_str(req, "X-Filename", name, sizeof(name));
    size_t total = req->content_len;
    if (total == 0 || total > kMaxDoc) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, kobento_net::errSizeBody().c_str());
    }
    Received r;
    r.name = name;
    r.bytes.resize(total);   // > 4 KB, so it lands in PSRAM
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, (char*) r.bytes.data() + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    ESP_LOGI(TAG, "received '%s' (%u bytes)", name, (unsigned) got);
    {
        std::lock_guard<std::mutex> g(sLock);
        if (sInbox.size() > 4) sInbox.pop_front();
        sInbox.push_back(std::move(r));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, kobento_net::sendOkBody(got).c_str());
}

// POST /named, body "name=value" lines: sets the documents' named variables, as if the
// player had (rcstage::setNamed). Lets a hub push to the display, e.g. Home Assistant's
// rest_command when the display's network can't open connections to the hub.
static esp_err_t onNamed(httpd_req_t* req) {
    const size_t total = req->content_len;
    if (total == 0 || total > 8192) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, kobento_net::errSizeBody().c_str());
    }
    std::string body(total, '\0');
    size_t got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body.data() + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    // Only changes go on: a hub that resends everything (a heartbeat) mustn't make an e-paper
    // panel redraw. The httpd has one task, so the map needs no lock. The "name=value" parsing
    // is kobento-net's (shared with the other language clients).
    static std::map<std::string, std::string> sPushed;
    int count = 0;
    for (auto& kv : kobento_net::parseNamed(body)) {
        auto it = sPushed.find(kv.first);
        if (it != sPushed.end() && it->second == kv.second) continue;
        rcstage::setNamed(kv.first, kv.second);
        sPushed[kv.first] = kv.second;
        count++;
    }
    if (count) ESP_LOGI(TAG, "named: %d changed", count);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, kobento_net::namedOkBody(count).c_str());
}

static void startServices() {
    if (!sHttp) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = CONFIG_RCPLAYER_KOBENTO_PORT;
        cfg.ctrl_port = CONFIG_RCPLAYER_KOBENTO_PORT + 1;
        cfg.stack_size = 8192;
        if (httpd_start(&sHttp, &cfg) == ESP_OK) {
            httpd_uri_t info = {.uri = "/info", .method = HTTP_GET, .handler = onInfo, .user_ctx = nullptr};
            httpd_uri_t send = {.uri = "/send", .method = HTTP_POST, .handler = onSend, .user_ctx = nullptr};
            httpd_register_uri_handler(sHttp, &info);
            httpd_register_uri_handler(sHttp, &send);
            httpd_uri_t named = {.uri = "/named", .method = HTTP_POST, .handler = onNamed, .user_ctx = nullptr};
            httpd_register_uri_handler(sHttp, &named);
        }
    }
    static bool mdnsUp = false;
    if (!mdnsUp && mdns_init() == ESP_OK) {
        mdns_hostname_set(CONFIG_RCPLAYER_DEVICE_NAME);
        mdns_txt_item_t txt[] = {{"v", "1"}, {"name", CONFIG_RCPLAYER_DEVICE_NAME}};
        mdns_service_add(CONFIG_RCPLAYER_DEVICE_NAME, "_kobento", "_tcp", CONFIG_RCPLAYER_KOBENTO_PORT, txt, 2);
        // Legacy alias, so senders that still discover by the old rcNet service name find us.
        mdns_service_add(CONFIG_RCPLAYER_DEVICE_NAME, "_rcnet", "_tcp", CONFIG_RCPLAYER_KOBENTO_PORT, txt, 2);
        mdnsUp = true;
    }
}

// ── Wi-Fi events ─────────────────────────────────────────────────────────────────────────

static void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        post({Op::Auto, ""});
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        sConnected = false;
        if (sAwaitLeave) { xSemaphoreGive(sLeft); return; }
        post({Op::Disconnected, "", ((wifi_event_sta_disconnected_t*) data)->reason});
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = (ip_event_got_ip_t*) data;
        snprintf(sIp, sizeof(sIp), IPSTR, IP2STR(&e->ip_info.ip));
        sConnected = true;
        ESP_LOGI(TAG, "connected, ip %s; kobento-net on port %d as '%s'", sIp, CONFIG_RCPLAYER_KOBENTO_PORT,
                 CONFIG_RCPLAYER_DEVICE_NAME);
        startServices();
        post({Op::GotIp, ""});
    }
}

void start() {
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    board::radioPower();
#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_MCU
    // A radio co-processor must be powered before esp_hosted talks to it, so its
    // before-app_main auto-init is off (the board's sdkconfig) and the link comes up here.
    if (esp_hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(TAG, "esp_hosted: no link to the radio co-processor");
        return;
    }
#endif
    loadAll();
    sLeft = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(wifiTask, "wifi_mgr", 6144, nullptr, 4, &sTask, 0);
    esp_netif_init();
    esp_event_loop_create_default();
    sNetif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sNetif, CONFIG_RCPLAYER_DEVICE_NAME);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onEvent, nullptr);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (is the C6 answering?)", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    sStarted = true;
}

}  // namespace net
