// The Muse gadget on a board with a screen: the SDK's gadget_ext.h hooks, drawing with
// RemoteCompose through rcstage. Hardware comes from the board (board/board.h).
//
//   early init  power the C6 radio, start the panel and a RemoteCompose document (rcstage)
//   status      Link's state as a banner over the document (or as named variables for a
//               document that shows it itself); a tap confirms pairing
//   device      battery, Wi-Fi, memory as RemoteCompose system/named variables (device_state)
//   actions     a document's hostAction(0, "name[=value]") / HostNamedAction drive the device
//   Wi-Fi       the Wi-Fi screen (wifi_settings): pick a network for the SDK to join; it
//               opens by itself when no network could be joined, or none is joined soon
//               after a restart or the power button
//   commands    rc.* so Muse can drive what the screen shows
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#include "gadget_ext.h"
extern "C" {
#include "config_store.h"
#include "identity.h"   // C headers without extern "C"
#include "wifi_known.h"
#include "wifi_mgr.h"
}
#include "rcstage/stage.h"
#include "board/board.h"
#include "rcstage/device_state.h"
#include "embedded_docs.h"
#include "muse_rc_internal.h"

static const char* TAG = "muse_rc";

static constexpr int kDocs = sizeof(kEmbeddedDocs) / sizeof(kEmbeddedDocs[0]);
static volatile led_state_t sState = LED_STATE_BOOT;
static bool sStage = false;

// ── Documents ───────────────────────────────────────────────────────────────────────────

static int findExact(const std::string& name) {
    for (int i = 0; i < kDocs; i++)
        if (name == kEmbeddedDocs[i].name) return i;
    return -1;
}

// A document may come in variants for particular screens, shown wherever <name> is asked
// for: "<name>_round" on a round screen, "<name>_light" (a white palette) on e-ink.
struct Variant { const char* suffix; bool mine; };
static constexpr Variant kVariants[] = {{"_round", board::kRound}, {"_light", board::kEink}};

static int findIndex(const std::string& name) {
    for (const Variant& v : kVariants) {
        if (!v.mine) continue;
        int i = findExact(name + v.suffix);
        if (i >= 0) return i;
    }
    return findExact(name);
}

const EmbeddedDoc* muserc::findDoc(const char* name) {
    int i = findIndex(name);
    return i >= 0 ? &kEmbeddedDocs[i] : nullptr;
}

const EmbeddedDoc* muserc::findDocExact(const char* name) {
    int i = findExact(name);
    return i >= 0 ? &kEmbeddedDocs[i] : nullptr;
}

bool muserc::stageUp() { return sStage; }
bool muserc::museConnected() { return sState == LED_STATE_WS_CONNECTED; }

// The long-press cycle shows each document once, as this board opens it: it skips the
// variants meant for other screens, and the plain document when this board has a variant.
static bool otherScreen(int i) {
    std::string n = kEmbeddedDocs[i].name;
    for (const Variant& v : kVariants) {
        const size_t len = strlen(v.suffix);
        if (n.size() > len && n.compare(n.size() - len, len, v.suffix) == 0) {
            n.resize(n.size() - len);
            break;
        }
    }
    return findIndex(n) != i;
}

static constexpr const char* kWifiDoc = "wifi_settings";
static bool isWifiDoc(const char* name) { return !strncmp(name, kWifiDoc, strlen(kWifiDoc)); }
static std::atomic<bool> sOnWifi{false};
static constexpr const char* kLauncherDoc = "launcher";
static bool isLauncherDoc(const char* name) { return !strncmp(name, kLauncherDoc, strlen(kLauncherDoc)); }
static std::atomic<bool> sOnLauncher{false};
static int sCurrent = 0, sPrevious = 0;   // player task
static int sSplash = 0;                   // the start screen's index
static void wifiScan();
static void applyBanner();
static void publishLauncher();
static void launchOpen(int row);
static void launchNextPage();
static void showPinned();

// Player task: open embedded document i. `step` (+1 or -1) skips the variants for other
// screens in that direction; 0 opens i as asked.
static void open(int i, int step = 0) {
    const int from = sCurrent;
    for (int tries = 0; tries < kDocs; tries++, i += step ? step : 1) {
        const int at = ((i % kDocs) + kDocs) % kDocs;
        if (step && otherScreen(at)) continue;
        const auto& d = kEmbeddedDocs[at];
        if (!rcstage::load(d.start, d.end - d.start, d.name, d.fit)) continue;
        sCurrent = at;
        if (sCurrent != from) sPrevious = from;
        sOnWifi = isWifiDoc(d.name);
        sOnLauncher = isLauncherDoc(d.name);
        applyBanner();   // none over the Wi-Fi screen or the launcher: they show the status
        if (sOnWifi) wifiScan();   // fresh list on the way in
        if (sOnLauncher) publishLauncher();
        return;
    }
}

// ── Serial console ('>' lines; tools/snap.py --prefix '>') ──────────────────────────────

static std::mutex sConsoleLock;
static std::vector<std::string> sConsoleLines;

extern "C" void gadget_ext_console(const char* line) {
    std::lock_guard<std::mutex> g(sConsoleLock);
    if (sConsoleLines.size() < 8) sConsoleLines.emplace_back(line);
}

// ── The Wi-Fi screen ─────────────────────────────────────────────────────────────────────
//
// The SDK owns Wi-Fi. This lists the networks it could join, as the RC player does
// (make_wifi.py: the same rows and states), and hands it the one picked
// (gadget_ext_wifi_join), which is then saved and joined at boot whenever the dev network
// isn't in range. Only open networks and saved ones can be joined: there's no password
// entry on the device.

static constexpr int kSlots = 10;   // rows in the document
static constexpr int kJoinMs = 20000;

struct Seen { std::string ssid; int rssi; bool secured; };
struct Saved { std::string ssid; bool secured; };
struct Entry { std::string ssid; int state; int signal; bool lock; };

static std::mutex sWifiLock;
static std::vector<Seen> sSeen;      // the last scan
static std::vector<Saved> sSaved;    // the SDK's saved networks, and the dev network
static std::string sJoining, sFailed;
static bool sScanning = false;
static std::vector<std::string> sRows;   // the SSIDs on screen, by row
static int sPage = 0;
static int sEntries = 0;                 // in the list, on every page
static std::atomic<bool> sJoined{false};     // picked and joined: go back (player task)
static std::atomic<bool> sNoWifi{false};     // no network joined: show the screen
static int64_t sReturnAt = 0;                // us; player task
// Roaming (the Wi-Fi task): carried from home to work while on, the SDK only retries the
// network it lost. After kRoamAfterUs without Wi-Fi, scan and join the strongest saved
// network in range; nothing in range: look again later, backing off.
static constexpr int64_t kRoamAfterUs = 30000000;
static constexpr int64_t kRoamBackoffMinUs = 30000000, kRoamBackoffMaxUs = 300000000;
static std::atomic<int64_t> sWifiDownSince{0};   // us; 0: up
// After a restart or the power button (not a timed wake) the start screen stays up while
// Wi-Fi joins. Joined: the pinned document. No network within kStartWaitUs, or a long press:
// the launcher. So a document that needs the network only shows once there is one. Player task.
enum class Start { None, Waiting, Done };
static Start sStart = Start::None;
static int64_t sStartDeadline = 0;
static constexpr int64_t kStartWaitUs = 60000000;   // the dev network, then the saved ones
// Paired (config_setup_complete), as of the SDK's last status. That reads NVS, which the
// player task (PSRAM stack on the S3) mustn't: flash reads assert there.
static std::atomic<bool> sSetupDone{false};
// The Wi-Fi screen opened by itself: go back to what was showing once a network is joined.
// The document's bytes, not an index: a pinned or downloaded one isn't embedded. Player task.
static bool sAutoWifi = false;
static std::shared_ptr<const std::vector<uint8_t>> sBackBytes;
static rcstage::DocInfo sBackInfo;

// The Wi-Fi task's work, one at a time.
struct WifiOp { bool join; char ssid[33]; int delayMs; };
static QueueHandle_t sWifiOps;

// A 44 dp header, then 37 dp rows. wifi_settings_light (e-ink) draws them 1.5x larger, with
// the status on a 24 dp line of its own under the header.
static constexpr float kWifiHeader = board::kEink ? 44 * 1.5f + 24 : 44;
static constexpr float kWifiRow = board::kEink ? 37 * 1.5f : 37;

static int rowsThatFit(int slots = kSlots) {
    if (board::kRound) return 3;   // wifi_settings_round: three rows across the circle
    const float dpHeight = board::kHeight / board::kDensity;
    return std::max(1, std::min(slots, (int) ((dpHeight - kWifiHeader) / kWifiRow)));
}

// Long names end in "…", cut on a UTF-8 boundary.
static std::string shorten(const std::string& s,
                           size_t max = board::kRound ? 17 : board::kEink ? 20 : 26) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && ((uint8_t) s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n) + "…";
}

static std::string connectedSsid() {
    wifi_ap_record_t ap = {};
    if (!wifi_mgr_is_connected() || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return {};
    return std::string((const char*) ap.ssid);
}

// Under sWifiLock. state: 1 connected, 2 connecting, 3 saved, 4 open (not saved), 5 saved
// but not in range, 6 couldn't connect, 7 needs a password (not saved). The current network
// first, then saved ones in range, open ones, saved ones out of range, and the rest.
static std::vector<Entry> entries(const std::string& current, std::string& status) {
    auto signal = [](int rssi) { return std::max(1, std::min(100, 2 * (rssi + 100))); };
    auto seen = [](const std::string& ssid) -> const Seen* {
        for (auto& s : sSeen)
            if (s.ssid == ssid) return &s;
        return nullptr;
    };
    auto saved = [](const std::string& ssid) {
        for (auto& s : sSaved)
            if (s.ssid == ssid) return true;
        return false;
    };
    const std::string& live = !sJoining.empty() ? sJoining : current;
    const int liveState = !sJoining.empty() ? 2 : 1;
    if (!sJoining.empty()) status = "Connecting to " + sJoining + "…";
    else if (!current.empty()) status = "Connected to " + current;
    else if (!sFailed.empty()) status = "Couldn't join " + sFailed;
    else status = sScanning ? "Looking for networks…" : "Not connected: pick a network";

    std::vector<Entry> now, inRange, open, away, locked;
    for (auto& n : sSaved) {
        const Seen* s = seen(n.ssid);
        Entry e{n.ssid, 3, s ? signal(s->rssi) : 0, n.secured};
        if (n.ssid == live) { e.state = liveState; now.push_back(e); }
        else if (n.ssid == sFailed) { e.state = 6; (s ? inRange : away).push_back(e); }
        else if (s) inRange.push_back(e);
        else { e.state = 5; away.push_back(e); }
    }
    for (auto& s : sSeen) {
        if (saved(s.ssid)) continue;
        Entry e{s.ssid, s.secured ? 7 : 4, signal(s.rssi), s.secured};
        if (s.ssid == live) { e.state = liveState; now.push_back(e); }
        else if (s.ssid == sFailed && !s.secured) { e.state = 6; open.push_back(e); }
        else (s.secured ? locked : open).push_back(e);
    }
    auto bySignal = [](const Entry& a, const Entry& b) { return a.signal > b.signal; };
    std::stable_sort(inRange.begin(), inRange.end(), bySignal);
    std::stable_sort(open.begin(), open.end(), bySignal);
    std::vector<Entry> all;
    for (auto* l : {&now, &inRange, &open, &away, &locked}) all.insert(all.end(), l->begin(), l->end());
    return all;
}

// Any task.
static void publishWifi() {
    const std::string current = connectedSsid();
    std::lock_guard<std::mutex> g(sWifiLock);
    std::string status;
    const std::vector<Entry> list = entries(current, status);
    const int fit = rowsThatFit();
    const int n = sEntries = (int) list.size();
    const int pages = std::max(1, (n + fit - 1) / fit);
    sPage = std::min(sPage, pages - 1);
    sRows.assign(kSlots, "");
    rcstage::setNamed("system.wifi_status", status);
    rcstage::setNamed("system.wifi_scanning", sScanning ? "1" : "0");
    rcstage::setNamed("system.wifi_pages", std::to_string(pages));
    rcstage::setNamed("system.wifi_page",
                      pages > 1 ? std::to_string(sPage + 1) + "/" + std::to_string(pages) : "");
    for (int r = 0; r < kSlots; r++) {
        const int i = sPage * fit + r;
        const bool on = r < fit && i < n;
        const std::string row = "system.wifi_" + std::to_string(r) + "_";
        if (on) sRows[r] = list[i].ssid;
        rcstage::setNamed(row + "ssid", on ? shorten(list[i].ssid) : "");
        rcstage::setNamed(row + "state", std::to_string(on ? list[i].state : 0));
        rcstage::setNamed(row + "signal", std::to_string(on ? list[i].signal : 0));
        rcstage::setNamed(row + "lock", on && list[i].lock ? "1" : "0");
    }
}

// Wi-Fi task (an internal-RAM stack: NVS).
static void loadSaved() {
    std::vector<Saved> out;
    if (auto* list = (wifi_known_list_t*) malloc(sizeof(wifi_known_list_t))) {
        const int n = wifi_known_load(list);
        for (int i = 0; i < n; i++) out.push_back({list->nets[i].ssid, list->nets[i].password[0] != 0});
        wifi_known_wipe(list);
        free(list);
    }
    const std::string dev = CONFIG_HOMEHUB_WIFI_SSID;
    if (!dev.empty() && std::none_of(out.begin(), out.end(), [&](const Saved& s) { return s.ssid == dev; }))
        out.push_back({dev, CONFIG_HOMEHUB_WIFI_PASSWORD[0] != 0});
    std::lock_guard<std::mutex> g(sWifiLock);
    sSaved = std::move(out);
}

// Scan (waiting out the SDK's own use of the radio, up to waitMs). False if it couldn't.
static bool scanInto(int waitMs) {
    std::vector<wifi_scan_entry_t> buf(25);
    int n = -1;
    for (int t = 0; (n = wifi_mgr_scan(buf.data(), (int) buf.size(), 0, nullptr)) < 0 && t * 500 < waitMs; t++)
        vTaskDelay(pdMS_TO_TICKS(500));
    if (n < 0) return false;
    std::lock_guard<std::mutex> g(sWifiLock);
    sSeen.clear();
    for (int i = 0; i < n; i++) sSeen.push_back({buf[i].ssid, buf[i].rssi, buf[i].auth_mode != WIFI_AUTH_OPEN});
    return true;
}

// Wi-Fi task, every few seconds: down a while, join the strongest saved network in range.
static void roam() {
    static int64_t next = 0, backoff = kRoamBackoffMinUs;
    const int64_t now = esp_timer_get_time();
    char joining[33];
    if (wifi_mgr_is_connected() || !wifi_mgr_get_netif()) {
        sWifiDownSince = 0;
        backoff = kRoamBackoffMinUs;
        next = 0;
        return;
    }
    if (!sWifiDownSince) sWifiDownSince = now;
    if (!sSetupDone || now - sWifiDownSince < kRoamAfterUs || now < next ||
        wifi_mgr_joining(joining, sizeof joining))
        return;
    loadSaved();
    std::vector<std::pair<int, std::string>> inRange;   // rssi, ssid
    if (scanInto(5000)) {
        std::lock_guard<std::mutex> g(sWifiLock);
        for (const Seen& seen : sSeen)
            if (std::any_of(sSaved.begin(), sSaved.end(), [&](const Saved& v) { return v.ssid == seen.ssid; }) &&
                std::none_of(inRange.begin(), inRange.end(), [&](auto& r) { return r.second == seen.ssid; }))
                inRange.push_back({seen.rssi, seen.ssid});
    }
    std::sort(inRange.begin(), inRange.end(), [](auto& a, auto& b) { return a.first > b.first; });
    bool ok = false;
    for (size_t i = 0; i < inRange.size() && i < 2 && !ok; i++) {
        ESP_LOGI(TAG, "roaming: Wi-Fi down %d s; joining %s (%d dBm)",
                 (int) ((now - sWifiDownSince) / 1000000), inRange[i].second.c_str(), inRange[i].first);
        ok = gadget_ext_wifi_join(inRange[i].second.c_str(), kJoinMs);
    }
    if (ok) {
        sWifiDownSince = 0;
        backoff = kRoamBackoffMinUs;
        next = 0;
    } else {
        if (inRange.empty()) ESP_LOGI(TAG, "roaming: no saved network in range; looking again in %d s", (int) (backoff / 1000000));
        next = esp_timer_get_time() + backoff;
        backoff = std::min(backoff * 2, kRoamBackoffMaxUs);
    }
    if (sOnWifi) publishWifi();
}

static void wifiTask(void*) {
    // The SDK starts Wi-Fi after early init.
    while (!wifi_mgr_get_netif()) vTaskDelay(pdMS_TO_TICKS(250));
    WifiOp op;
    for (;;) {
        if (xQueueReceive(sWifiOps, &op, pdMS_TO_TICKS(5000)) != pdTRUE) {
            roam();
            continue;
        }
        if (op.delayMs) vTaskDelay(pdMS_TO_TICKS(op.delayMs));
        loadSaved();
        if (!op.join) {
            {
                std::lock_guard<std::mutex> g(sWifiLock);
                sScanning = true;
            }
            publishWifi();
            // The SDK may have the radio for a join of its own: wait for it.
            scanInto(60000);
            std::lock_guard<std::mutex> g(sWifiLock);
            sScanning = false;
        } else {
            {
                std::lock_guard<std::mutex> g(sWifiLock);
                sJoining = op.ssid;
                sFailed.clear();
            }
            publishWifi();
            char other[33];
            for (int t = 0; t < 120 && wifi_mgr_joining(other, sizeof(other)); t++) vTaskDelay(pdMS_TO_TICKS(500));
            const bool ok = gadget_ext_wifi_join(op.ssid, kJoinMs);
            loadSaved();
            std::lock_guard<std::mutex> g(sWifiLock);
            sJoining.clear();
            if (ok) sJoined = true;
            else sFailed = op.ssid;
        }
        publishWifi();
    }
}

static void wifiStart() {
    static std::mutex lock;
    std::lock_guard<std::mutex> g(lock);
    if (sWifiOps) return;
    sWifiOps = xQueueCreate(4, sizeof(WifiOp));
    xTaskCreate(wifiTask, "rc_wifi", 6144, nullptr, 4, nullptr);
}

static void wifiOp(bool join, const std::string& ssid = {}, int delayMs = 0) {
    wifiStart();
    WifiOp op{join, {}, delayMs};
    snprintf(op.ssid, sizeof(op.ssid), "%s", ssid.c_str());
    if (xQueueSend(sWifiOps, &op, 0) != pdTRUE) ESP_LOGW(TAG, "Wi-Fi busy; dropped a %s", join ? "join" : "scan");
}

static void wifiScan() { wifiOp(false); }

// ── device.wifi ─────────────────────────────────────────────────────────────────────────
// Status, and the saved networks: add one with its password (so a network with a password
// can be joined later, at boot or by roaming), forget one, or join one now.

std::string muserc::wifiSsid() { return connectedSsid(); }

static cJSON* wifiOk(cJSON* p) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", p);
    return r;
}

cJSON* muserc::wifiCommand(cJSON* params) {
    auto str = [&](const char* k) -> const char* {
        cJSON* v = cJSON_GetObjectItem(params, k);
        return cJSON_IsString(v) ? v->valuestring : nullptr;
    };
    const std::string dev = CONFIG_HOMEHUB_WIFI_SSID;
    cJSON* p = cJSON_CreateObject();
    if (cJSON_GetObjectItem(params, "add")) {
        const char* ssid = str("add");
        const char* pass = str("password") ? str("password") : "";
        const size_t pl = strlen(pass);
        if (!ssid || !*ssid || strlen(ssid) > WIFI_KNOWN_SSID_MAX) {
            cJSON_Delete(p);
            return gadget_ext_error("bad_param", "add: an SSID of 1-32 bytes");
        }
        if (pl && (pl < 8 || pl > WIFI_KNOWN_PASS_MAX)) {
            cJSON_Delete(p);
            return gadget_ext_error("bad_param", "password: 8-64 characters, or empty for an open network");
        }
        if (!wifi_known_remember(ssid, pass, cJSON_IsTrue(cJSON_GetObjectItem(params, "hidden")) ? 1 : 0)) {
            cJSON_Delete(p);
            return gadget_ext_error("unavailable", "could not save it");
        }
        ESP_LOGI(TAG, "device.wifi: saved %s", ssid);
        cJSON_AddStringToObject(p, "saved", ssid);
    } else if (const char* f = str("forget")) {
        if (f == dev) {
            cJSON_Delete(p);
            return gadget_ext_error("bad_param", "the firmware's own network can't be forgotten");
        }
        if (!wifi_known_forget(f)) {
            cJSON_Delete(p);
            return gadget_ext_error("unavailable", "could not save the change");
        }
        cJSON_AddStringToObject(p, "forgot", f);
    } else if (const char* j = str("join")) {
        // After the reply: the agent's session runs over the network being left.
        loadSaved();
        bool known;
        {
            std::lock_guard<std::mutex> g(sWifiLock);
            known = std::any_of(sSaved.begin(), sSaved.end(), [&](const Saved& v) { return v.ssid == j; });
        }
        if (!known) {
            cJSON_Delete(p);
            return gadget_ext_error("bad_param", "not a saved network: add it first");
        }
        wifiOp(true, j, 1500);
        cJSON_AddStringToObject(p, "joining", j);
        cJSON_AddStringToObject(p, "note", "the session drops and comes back on the new network (about 15 s)");
    }
    // The state, after any change.
    loadSaved();
    const std::string ssid = connectedSsid();
    cJSON_AddBoolToObject(p, "connected", !ssid.empty());
    if (!ssid.empty()) {
        cJSON_AddStringToObject(p, "ssid", ssid.c_str());
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            cJSON_AddNumberToObject(p, "rssi", ap.rssi);
            cJSON_AddNumberToObject(p, "channel", ap.primary);
        }
    } else if (int64_t down = sWifiDownSince.load()) {
        cJSON_AddNumberToObject(p, "down_s", (double) ((esp_timer_get_time() - down) / 1000000));
    }
    cJSON* saved = cJSON_AddArrayToObject(p, "saved");
    std::lock_guard<std::mutex> g(sWifiLock);
    for (const Saved& v : sSaved) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", v.ssid.c_str());
        cJSON_AddBoolToObject(o, "password", v.secured);
        if (v.ssid == dev) cJSON_AddBoolToObject(o, "firmware", true);
        cJSON_AddItemToArray(saved, o);
    }
    return wifiOk(p);
}

// ── Host actions ────────────────────────────────────────────────────────────────────────
//
// hostAction(0, "name[=value]") and HostNamedAction(name, value) mean the same thing here.
// Player task.

static int sBacklight = 100;

static void runHostAction(const std::string& name, const std::string& value) {
    if (name == "system.backlight") {
        // A fraction 0..1, or "cycle" through 100 -> 60 -> 30 %.
        int pct = value == "cycle" ? (sBacklight > 60 ? 60 : sBacklight > 30 ? 30 : 100)
                                   : (int) (std::strtof(value.c_str(), nullptr) * 100);
        sBacklight = pct < 5 ? 5 : pct > 100 ? 100 : pct;
        board::setBrightness(sBacklight);
        rcstage::setNamed("system.backlight", std::to_string(sBacklight / 100.0f));
    } else if (name == "muse.press") {
        gadget_ext_press();
    } else if (name == "doc.show") {
        int i = findIndex(value);
        if (i >= 0) open(i);
        else ESP_LOGW(TAG, "no document '%s'", value.c_str());
    } else if (name == "doc.next") {
        open(sCurrent + 1, 1);
    } else if (name == "doc.back") {
        open(sPrevious);
    } else if (name == "wifi.join") {
        std::string ssid;
        {
            std::lock_guard<std::mutex> g(sWifiLock);
            int r = atoi(value.c_str());
            if (r >= 0 && r < (int) sRows.size()) ssid = sRows[r];
        }
        if (!ssid.empty()) wifiOp(true, ssid);
    } else if (name == "launch.open") {
        launchOpen(atoi(value.c_str()));
    } else if (name == "launch.back") {
        showPinned();
    } else if (name == "launch.page") {
        launchNextPage();
    } else if (name == "wifi.scan") {
        wifiScan();
    } else if (name == "wifi.page") {
        {
            std::lock_guard<std::mutex> g(sWifiLock);
            if (++sPage * rowsThatFit() >= sEntries) sPage = 0;
        }
        publishWifi();
    } else {
        ESP_LOGW(TAG, "unknown host action '%s'", name.c_str());
    }
}

static void onHostAction(int id, const std::string& metadata) {
    if (id != 0) return;
    size_t eq = metadata.find('=');
    runHostAction(metadata.substr(0, eq), eq == std::string::npos ? "" : metadata.substr(eq + 1));
}

static void onNamedAction(const std::string& name, int, const std::string& value) {
    runHostAction(name, value);
}

// The document rc.pin kept (and persisted variable values), once the SDK has NVS up. A
// task of its own with an internal-RAM stack: flash reads turn the cache off, and the
// player task's stack may be in PSRAM (the PaperS3).
// The pinned document, loaded from flash (pinTask at boot, showPinTask for the launcher's
// Back), posted to the player task. doc null: nothing pinned.
struct PinMail {
    bool ready = false;
    bool show = false;   // Back: show it now (the start screen if nothing is pinned)
    std::unique_ptr<muserc::Pinned> doc;
};
static std::mutex sPinLock;
static PinMail sPinMail;
static bool sPinResolved = false;                    // player task: the boot load is in
static std::unique_ptr<muserc::Pinned> sHeldPin;     // player task: for when Wi-Fi joins

static bool sPinnedAtBoot = false;   // early init showed it already

static void postPin(muserc::PinLoad r, muserc::Pinned& pinned, bool show) {
    std::lock_guard<std::mutex> g(sPinLock);
    sPinMail.ready = true;
    sPinMail.show = show;
    sPinMail.doc = r == muserc::PinLoad::Loaded ? std::make_unique<muserc::Pinned>(std::move(pinned)) : nullptr;
}

static void pinTask(void*) {
    muserc::Pinned pinned;
    muserc::PinLoad r;
    while ((r = muserc::loadPinned(pinned)) == muserc::PinLoad::NotYet) vTaskDelay(pdMS_TO_TICKS(100));
    muserc::sleepLoad();
    muserc::sleepStart();
    wifiStart();   // and roaming with it
    if (sPinnedAtBoot) r = muserc::PinLoad::None;
    postPin(r, pinned, false);
    vTaskDelete(nullptr);
}

static void showPinTask(void*) {
    muserc::Pinned pinned;
    muserc::PinLoad r;
    while ((r = muserc::loadPinned(pinned)) == muserc::PinLoad::NotYet) vTaskDelay(pdMS_TO_TICKS(100));
    postPin(r, pinned, true);
    vTaskDelete(nullptr);
}

// Player task.
static void showPin(muserc::Pinned& pin) {
    if (!rcstage::load(pin.bytes.data(), pin.bytes.size(), pin.name.c_str(), pin.fit)) return;
    const int i = findExact(pin.name);   // a built-in one: for sCurrent
    if (i >= 0) {
        if (i != sCurrent) sPrevious = sCurrent;
        sCurrent = i;
    }
    sOnWifi = false;
    sOnLauncher = false;
    applyBanner();
}

// ── The launcher (launcher.rc, tools/splash/make_launcher.py) ──────────────────────────
// Wi-Fi, then the pinned document, then every document built in. Player task.

struct LaunchEntry { int kind; int doc; };   // kind 1 Wi-Fi, 2 a document, 3 the pinned one; doc -1: downloaded
static constexpr int kLaunchSlots = 16;   // rows in launcher.rc (make_launcher.py ROWS)
static std::vector<LaunchEntry> sLaunch;
static int sLaunchPage = 0;
static std::string sLaunchWifi, sLaunchStatus;   // as last set

static bool systemDoc(const char* name) {
    return isWifiDoc(name) || isLauncherDoc(name) || !strcmp(name, "muse_splash");
}

static std::string pretty(std::string n) {
    for (char& c : n)
        if (c == '_') c = ' ';
    if (!n.empty() && n[0] >= 'a' && n[0] <= 'z') n[0] = (char) (n[0] - 'a' + 'A');
    return n;
}

static void launcherStatus() {
    char joining[33];
    const std::string ssid = connectedSsid();
    std::string wifi = !ssid.empty() ? ssid
                     : wifi_mgr_joining(joining, sizeof joining) ? std::string("Connecting to ") + joining + "…"
                     : "Not connected";
    const led_state_t st = sState;
    const char* muse = st == LED_STATE_WS_CONNECTED ? "Muse connected"
                     : ssid.empty() ? "Muse offline" : "Connecting to Muse…";
    std::string status = (ssid.empty() ? std::string("No Wi-Fi") : "Wi-Fi " + ssid) + " · " + muse;
    if (wifi != sLaunchWifi) rcstage::setNamed("system.launch_wifi", sLaunchWifi = wifi);
    if (status != sLaunchStatus) rcstage::setNamed("system.launch_status", sLaunchStatus = status);
}

static void publishLauncher() {
    const std::string pinned = muserc::pinnedNow();
    const int pinIndex = pinned.empty() ? -1 : findExact(pinned);
    sLaunch.clear();
    sLaunch.push_back({1, -1});
    if (!pinned.empty()) sLaunch.push_back({3, pinIndex});
    for (int i = 0; i < kDocs; i++)
        if (i != pinIndex && !otherScreen(i) && !systemDoc(kEmbeddedDocs[i].name)) sLaunch.push_back({2, i});
    const int rows = rowsThatFit(kLaunchSlots);
    const int pages = std::max(1, ((int) sLaunch.size() + rows - 1) / rows);
    if (sLaunchPage >= pages) sLaunchPage = 0;
    rcstage::setNamed("system.launch_pages", std::to_string(pages));
    rcstage::setNamed("system.launch_page",
                      pages > 1 ? std::to_string(sLaunchPage + 1) + "/" + std::to_string(pages) : "");
    for (int r = 0; r < kLaunchSlots; r++) {
        const int at = sLaunchPage * rows + r;
        const std::string row = "system.launch_" + std::to_string(r) + "_";
        if (r >= rows || at >= (int) sLaunch.size()) {
            rcstage::setNamed(row + "kind", "0");
            continue;
        }
        const LaunchEntry& e = sLaunch[at];
        std::string name, detail;
        if (e.kind == 1) name = "Wi-Fi";
        else if (e.doc >= 0) {
            name = pretty(kEmbeddedDocs[e.doc].name);
            detail = e.kind == 3 ? "Pinned" : "Built in";
        } else {
            name = pretty(pinned);
            detail = "Pinned · downloaded";
        }
        rcstage::setNamed(row + "name", shorten(name, board::kEink ? 24 : 30));
        rcstage::setNamed(row + "detail", detail);
        rcstage::setNamed(row + "kind", std::to_string(e.kind));
    }
    sLaunchWifi.clear();
    sLaunchStatus.clear();
    launcherStatus();
}

static void launchNextPage() {
    sLaunchPage++;
    publishLauncher();
}

static void showPinned() {
    if (xTaskCreate(showPinTask, "rc_pin", 4096, nullptr, 3, nullptr) != pdPASS) ESP_LOGW(TAG, "no task for Back");
}

static void openLauncher() {
    if (sStart == Start::Waiting) {
        sStart = Start::Done;
        sHeldPin.reset();
    }
    if (sOnLauncher) return;
    sLaunchPage = 0;
    const int i = findIndex(kLauncherDoc);
    if (i >= 0) open(i);
    else ESP_LOGW(TAG, "no launcher document");
}

static void launchOpen(int row) {
    const int at = sLaunchPage * rowsThatFit(kLaunchSlots) + row;
    if (row < 0 || at >= (int) sLaunch.size()) return;
    const LaunchEntry& e = sLaunch[at];
    if (e.kind == 1) {
        if (int i = findIndex(kWifiDoc); i >= 0) open(i);   // its Done comes back here
    } else if (e.kind == 3) {
        showPinned();   // from flash: with its persisted values
    } else if (e.doc >= 0) {
        open(e.doc);
    }
}

// Player task.
static void tick() {
    PinMail mail;
    {
        std::lock_guard<std::mutex> g(sPinLock);
        std::swap(mail, sPinMail);
    }
    if (mail.ready && mail.show) {
        // Back: the pinned document, or the start screen when nothing is pinned.
        if (mail.doc) showPin(*mail.doc);
        else open(sSplash);
    } else if (mail.ready) {
        sPinResolved = true;
        if (!mail.doc) {
        } else if (sStart == Start::Waiting) {
            sHeldPin = std::move(mail.doc);   // shown once Wi-Fi joins
        } else if (sAutoWifi) {
            // The Wi-Fi screen opened first: the pinned document is what it goes back to.
            const int i = findExact(mail.doc->name);
            sBackInfo.name = mail.doc->name;
            sBackInfo.fit = mail.doc->fit;
            sBackBytes = std::make_shared<const std::vector<uint8_t>>(std::move(mail.doc->bytes));
            if (i >= 0) sPrevious = i;
        } else if (!sOnLauncher && !sOnWifi) {
            showPin(*mail.doc);
        }
    }
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> g(sConsoleLock);
        lines.swap(sConsoleLines);
    }
    for (auto& l : lines) {
        auto arg = [&](const char* cmd) -> const char* {
            size_t n = strlen(cmd);
            return l.compare(0, n, cmd) == 0 ? l.c_str() + n : nullptr;
        };
        const char* a;
        if (muserc::consoleApi(l)) {}
        else if (l == "snap") board::dumpFrame(board::kWidth > 400 ? 2 : 1);   // ~360 px wide
        else if (l == "tap") gadget_ext_press();
        else if (l == "stats") rcstage::requestStats();
        else if (l == "vars") for (auto& v : rcstage::namedVariables()) printf("var %s\n", v.c_str());
        else if (l == "doc") {
            // The showing document, base64 in 48-byte lines (tools/getdoc.py saves it).
            const std::vector<uint8_t>& d = rcstage::currentBytes();
            printf("\nDOC BEGIN %u %s\n", (unsigned) d.size(), rcstage::currentName().c_str());
            unsigned char b64[72];
            for (size_t i = 0; i < d.size(); i += 48) {
                size_t olen = 0;
                mbedtls_base64_encode(b64, sizeof(b64), &olen, d.data() + i, std::min<size_t>(48, d.size() - i));
                printf("%.*s\n", (int) olen, b64);
            }
            printf("DOC END\n");
        }
        else if (board::console(l)) {}
        else if (l == "sys") {
            rcstage::DeviceState v = rcstage::deviceState();
            const board::Power& p = v.power;
            printf("battery %.2f V %.0f mA level %d power %d charging %d headphones %d "
                   "rssi %d mem %.1f MB temp %.1f C\n", p.batteryVolts, p.batteryMilliamps,
                   p.batteryLevel, p.powerState, p.charging, p.headphones, v.wifiRssi,
                   v.memoryFreeMb, v.temperatureC);
        } else if ((a = arg("act "))) onHostAction(0, a);
        else if ((a = arg("show "))) {
            int i = findIndex(a);
            if (i >= 0) open(i);
            else printf("no document '%s'\n", a);
        } else if ((a = arg("set "))) {
            std::string kv(a);
            size_t sp = kv.find(' ');
            if (sp != std::string::npos) rcstage::setNamed(kv.substr(0, sp), kv.substr(sp + 1));
        } else if ((a = arg("banner"))) {
            rcstage::setBanner(*a ? a + 1 : "", "", 0xFF42A5F5);
        } else printf("? %s (snap, doc, tap, stats, vars, sys [history], expander, act NAME[=VALUE], show NAME, set NAME VALUE, "
                      "banner [TEXT], api COMMAND [JSON])\n", l.c_str());
    }
    // Hardware buttons step through the documents: forward like a long press, or back.
    if (rcstage::lastTouch().down) muserc::noteActivity();
    if (int step = board::buttonPressed()) {
        muserc::noteActivity();
        rcstage::pushEvent({0, 0, "button", 0, 0, {}, step > 0 ? "+1" : "-1"});
        open(sCurrent + step, step);
    }
    const int64_t now = esp_timer_get_time();
    // Started by hand: the start screen until Wi-Fi joins (then the pinned document) or
    // fails (then the launcher). Not before pairing: the pairing banner has the screen then.
    if (sStart == Start::Waiting) {
        if (rcstage::lastTouch().down && sSetupDone) {
            // A touch on the start screen: the launcher. (A finger already there at power-up
            // is invisible: the GT911 calibrates it away.) The rest of the touch is ours.
            ESP_LOGI(TAG, "start: touched; the launcher");
            rcstage::swallowTouch();
            openLauncher();
        } else if (rcstage::currentName() != kEmbeddedDocs[sSplash].name) {
            sStart = Start::Done;   // something else took the screen (an agent's rc.show)
            sHeldPin.reset();
        } else if (wifi_mgr_is_connected() && sPinResolved) {
            ESP_LOGI(TAG, "start: Wi-Fi up; %s", sHeldPin ? "the pinned document" : "nothing pinned");
            sStart = Start::Done;
            if (sHeldPin) showPin(*sHeldPin);
            sHeldPin.reset();
        } else if (sNoWifi.exchange(false) || now >= sStartDeadline) {
            sStart = Start::Done;
            sHeldPin.reset();
            if (sSetupDone) {
                ESP_LOGI(TAG, "start: no Wi-Fi; the launcher");
                openLauncher();
            }
        }
    }
    // The launcher's Wi-Fi and Muse lines.
    static int64_t nextStatus = 0;
    if (sOnLauncher && now >= nextStatus) {
        nextStatus = now + 1000000;
        launcherStatus();
    }
    // No network could be joined: open the Wi-Fi screen to pick one (the launcher, which
    // has Wi-Fi at the top, already offers it).
    if (sNoWifi.exchange(false) && !sOnWifi && !sOnLauncher) {
        int i = findIndex(kWifiDoc);
        if (i >= 0) {
            ESP_LOGI(TAG, "no Wi-Fi: opening the Wi-Fi screen");
            sBackBytes = rcstage::docBytes();
            sBackInfo = rcstage::docInfo();
            open(i);
            sAutoWifi = sOnWifi;
        }
    }
    if (!sOnWifi) sAutoWifi = false;   // left it by hand
    // Joined the network picked on the Wi-Fi screen, or (opened by itself) any network:
    // show "Connected" briefly, then go back.
    if (sJoined.exchange(false)) sReturnAt = now + 1500000;
    if (sAutoWifi && !sReturnAt && wifi_mgr_is_connected()) sReturnAt = now + 1500000;
    if (sReturnAt && now >= sReturnAt) {
        sReturnAt = 0;
        if (sOnWifi) {
            if (sAutoWifi && sBackBytes &&
                rcstage::load(sBackBytes->data(), sBackBytes->size(), sBackInfo.name.c_str(), sBackInfo.fit)) {
                std::swap(sCurrent, sPrevious);
                sOnWifi = false;
                applyBanner();
            } else {
                open(sPrevious);
            }
        }
        sAutoWifi = false;
        sBackBytes.reset();
    }
}

// ── Early init ──────────────────────────────────────────────────────────────────────────

static void onTap(float, float) {
    // Boards without a setup button (HOMEHUB_BUTTON=n): a tap stands in for it.
    led_state_t s = sState;
    if (s == LED_STATE_PAIRING_CONFIRM_REQUIRED || s == LED_STATE_SETUP_IDLE || s == LED_STATE_UNPAIRED) {
        ESP_LOGI(TAG, "tap: setup press");
        gadget_ext_press();
    }
}

extern "C" void gadget_ext_early_init(void) {
    // A radio co-processor must be powered before the SDK's hosted_radio_start().
    board::radioPower();
    if (!board::displayInit()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    rcstage::Hooks hooks;
    hooks.tick = tick;
    hooks.tap = onTap;
    hooks.longPress = [] { openLauncher(); };   // every document on the device
    hooks.hostAction = onHostAction;
    hooks.namedAction = onNamedAction;
    // The start screen (tools/splash/make_splash.py); a pinned document replaces it once
    // NVS is up (pinTask). Woken by device.sleep's timer, the pinned document straight away,
    // with the time from the RTC: e-ink would show the start screen and then the document.
    sCurrent = sPrevious = sSplash = std::max(0, findIndex("muse_splash"));
    const char* woke = board::wakeReason();
    const bool timed = !strcmp(woke, "timer") || !strcmp(woke, "rtc");
    muserc::Pinned pinned;
    if (timed) rcstage::clockFromRtc();   // after a power-off the system clock starts at 1970
    if (timed && nvs_flash_init() == ESP_OK && muserc::loadPinned(pinned) == muserc::PinLoad::Loaded) {
        if (int i = findExact(pinned.name); i >= 0) sCurrent = sPrevious = i;
        sStage = rcstage::start(hooks, pinned.bytes.data(), pinned.bytes.size(), pinned.name.c_str(), pinned.fit);
        sPinnedAtBoot = sStage;
    }
    if (!sStage) {
        const auto& d = kEmbeddedDocs[sCurrent];
        sStage = rcstage::start(hooks, d.start, d.end - d.start, d.name);
    }
    // Not on a timed wake: it may go back to sleep without starting, and e-ink would keep
    // the banner on screen while it sleeps.
    if (!timed) {
        rcstage::setBanner("Starting", "Muse Gadget", 0xFF9E9E9E);
        sStart = Start::Waiting;
        sStartDeadline = esp_timer_get_time() + kStartWaitUs;
    }
    rcstage::setNamed("system.backlight", "1");
    rcstage::setNamed("system.muse", "1");   // the start screen shows Muse + RC
    if (sStage) {
        rcstage::deviceStateStart();
        if (timed) {
            muserc::sleepLoad();
            muserc::wakeLocally();   // sleeps again unless a check-in is due
        }
        xTaskCreate(pinTask, "rc_pin", 4096, nullptr, 3, nullptr);
    }
    ESP_LOGI(TAG, "started: internal RAM free %u",
             (unsigned) heap_caps_get_free_size(MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL));
}

// ── Status ──────────────────────────────────────────────────────────────────────────────

static std::string sTitle;

extern "C" void gadget_ext_title(const char* title) {
    sTitle = title ? title : "";
    rcstage::setNamed("muse.agent", sTitle);
}

// muse.link for documents: -1 error, 0 not paired, 1 connecting, 2 connected.
static int linkLevel(led_state_t s) {
    switch (s) {
        case LED_STATE_WS_CONNECTED: return 2;
        case LED_STATE_ERROR: return -1;
        case LED_STATE_SETUP_IDLE: case LED_STATE_UNPAIRED: case LED_STATE_BLE_ADVERTISING:
        case LED_STATE_BLE_CONNECTED: case LED_STATE_PAIRING_CONFIRM_REQUIRED: return 0;
        case LED_STATE_WIFI_CONNECTED: return config_setup_complete() ? 1 : 0;
        default: return 1;
    }
}

// The banner for Link's state, kept so it comes back after the Wi-Fi screen.
static std::mutex sBannerLock;
static std::string sBannerTitle = "Starting", sBannerDetail = "Muse Gadget";
static uint32_t sBannerColor = 0xFF9E9E9E;

static void setStatusBanner(const std::string& title, const std::string& detail = {}, uint32_t argb = 0) {
    {
        std::lock_guard<std::mutex> g(sBannerLock);
        sBannerTitle = title;
        sBannerDetail = detail;
        sBannerColor = argb;
    }
    applyBanner();
}

static void applyBanner() {
    std::lock_guard<std::mutex> g(sBannerLock);
    if (sOnWifi || sOnLauncher) rcstage::setBanner("");
    else rcstage::setBanner(sBannerTitle, sBannerDetail, sBannerColor);
}

extern "C" void gadget_ext_status(led_state_t state) {
    // After a confirmed pairing the SDK reports WIFI_CONNECTING while it waits for the phone
    // to send Wi-Fi credentials; say so rather than "joining".
    const led_state_t prev = sState;
    const bool awaitingPhone = state == LED_STATE_WIFI_CONNECTING &&
        (prev == LED_STATE_PAIRING_CONFIRM_REQUIRED || prev == LED_STATE_BLE_CONNECTED);
    sState = state;
    sSetupDone = config_setup_complete();
    if (!sStage) return;
    const std::string name = identity_ble_name() ? identity_ble_name() : "";
    rcstage::setNamed("muse.link", std::to_string(linkLevel(state)));
    rcstage::setNamed("muse.device", name);
    constexpr uint32_t kOrange = 0xFFFF9800, kBlue = 0xFF42A5F5, kAmber = 0xFFFFC107,
                       kRed = 0xFFF44336, kGrey = 0xFF9E9E9E;
    switch (state) {
        case LED_STATE_BOOT: setStatusBanner("Starting", "Muse Gadget", kGrey); break;
        case LED_STATE_SETUP_IDLE:
        case LED_STATE_UNPAIRED: setStatusBanner("Not paired", "Tap to start pairing", kOrange); break;
        case LED_STATE_BLE_ADVERTISING: setStatusBanner("Pair in the Muse app", name, kOrange); break;
        case LED_STATE_BLE_CONNECTED: setStatusBanner("Phone connected", name, kBlue); break;
        case LED_STATE_PAIRING_CONFIRM_REQUIRED:
            setStatusBanner("Tap the screen to confirm", "Pairing with your phone", kAmber);
            break;
        case LED_STATE_WIFI_CONNECTING:
            if (awaitingPhone) setStatusBanner("Paired", "Finish setup in the Muse app", kBlue);
            else setStatusBanner("Joining Wi-Fi", "", kBlue);
            break;
        case LED_STATE_WIFI_CONNECTED:
            // With the dev Wi-Fi override the network comes up before pairing.
            if (!config_setup_complete()) setStatusBanner("Pair in the Muse app", name, kOrange);
            else setStatusBanner("Connecting to Muse", sTitle, kBlue);
            break;
        case LED_STATE_AUTH_OK:
        case LED_STATE_VM_SWITCHING:
        case LED_STATE_VM_OK: setStatusBanner("Connecting to Muse", sTitle, kBlue); break;
        case LED_STATE_WS_CONNECTED: setStatusBanner(""); break;   // the document has the screen
        case LED_STATE_WS_DISCONNECTED: setStatusBanner("Reconnecting", sTitle, kAmber); break;
        case LED_STATE_ERROR:
            if (!wifi_mgr_is_connected()) {
                // No network joined (the dev network out of range, say): pick one.
                setStatusBanner("No Wi-Fi", "Pick a network", kRed);
                sNoWifi = true;
            } else {
                setStatusBanner("Something went wrong", "See the serial log", kRed);
            }
            break;
    }
    if (sOnWifi) publishWifi();
}
