// device.sleep: sleep for a while, or keep sleeping between the document's frames.
//
// The document says when it next needs a frame (rcstage::nextFrameMs: the engine's schedule
// from the time variables it reads and its WakeIn ops, as the Java player's
// RemoteComposeView reads needsRepaint()). With auto on, the board sleeps until then:
//   - a static document (nothing scheduled) sleeps until the next Muse check-in;
//   - a minute hand wakes each minute, draws, and sleeps again;
//   - a document with WakeIn(300) wakes every 5 minutes.
// Muse can't reach a sleeping board, so it checks in every check_in_s: a full boot (Deep and
// Off) or a reconnect (Light), then awake_s awake for the agent before sleeping again. The
// wakes in between are local: in early init, before the radios, the board draws the
// document and goes back to sleep (wakeLocally).
//
// When it stays awake regardless:
//   - on USB power (no battery to save; it's being worked on);
//   - for kManualAwakeS after a wake that wasn't the timer (the power button, plugging it in,
//     a reset): someone is there;
//   - with hold on, from connecting at a check-in until the agent sends device.sleep
//     {"resume": true} (or hold_max_s passes), so the agent can be sure of reaching it.
//
// Deep and Off lose RAM: each wake starts the document afresh (the pinned one, with the
// persisted values). A document that keeps history (a graph) needs Light.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "gadget_ext.h"
#include "board/board.h"
#include "rcstage/device_state.h"
#include "rcstage/stage.h"
#include "muse_rc_internal.h"

static const char* TAG = "muse_sleep";

namespace muserc {

struct Policy {
    bool on = false;                      // auto
    board::Sleep mode = board::Sleep::Off;
    int checkInS = 900;                   // connect to Muse this often
    int awakeS = 60;                      // stay awake this long after connecting, or after activity
    bool hold = false;                    // at a check-in, stay awake until device.sleep resume
    int holdMaxS = 3600;                  // ... or this long after connecting; 0: no limit
};

static std::mutex sLock;
static Policy sPolicy;
static int64_t sCheckInDue = 0;           // epoch s; 0: now
static std::atomic<int64_t> sActivityUs{0};
static std::atomic<bool> sResume{false};    // device.sleep resume: end the check-in or the manual window
static std::atomic<bool> sHolding{false};   // connected at a check-in, waiting for resume

// A local wake costs about this much before the frame is drawn (boot, document, sampler):
// the board wakes this much early. Shorter sleeps aren't worth it on Deep and Off.
static constexpr int kWakeCostS = 3;
static constexpr int kMinSleepS = 8;
static constexpr int kMinAwakeS = 60;       // an agent's turn takes longer than a few seconds
static constexpr int kManualAwakeS = 300;   // after the power button, plugging in or a reset

static const char* modeName(board::Sleep m) {
    return m == board::Sleep::Light ? "light" : m == board::Sleep::Deep ? "deep" : "off";
}

static bool parseMode(const char* s, board::Sleep& m) {
    if (!s || !*s || !strcmp(s, "off")) m = board::Sleep::Off;
    else if (!strcmp(s, "deep")) m = board::Sleep::Deep;
    else if (!strcmp(s, "light")) m = board::Sleep::Light;
    else return false;
    return true;
}

static void save() {
    nvs_handle_t h;
    if (nvs_open("rcpin", NVS_READWRITE, &h) != ESP_OK) return;
    char json[192];
    snprintf(json, sizeof json,
             "{\"auto\":%s,\"mode\":\"%s\",\"check_in_s\":%d,\"awake_s\":%d,\"hold\":%s,\"hold_max_s\":%d}",
             sPolicy.on ? "true" : "false", modeName(sPolicy.mode), sPolicy.checkInS, sPolicy.awakeS,
             sPolicy.hold ? "true" : "false", sPolicy.holdMaxS);
    nvs_set_str(h, "sleep", json);
    nvs_set_i64(h, "checkin", sCheckInDue);
    nvs_commit(h);
    nvs_close(h);
}

// After NVS is up (the SDK's, or early init's own on a timed wake).
void sleepLoad() {
    std::lock_guard<std::mutex> g(sLock);
    nvs_handle_t h;
    if (nvs_open("rcpin", NVS_READONLY, &h) != ESP_OK) return;
    char json[192];
    size_t n = sizeof json;
    if (nvs_get_str(h, "sleep", json, &n) == ESP_OK) {
        if (cJSON* j = cJSON_Parse(json)) {
            sPolicy.on = cJSON_IsTrue(cJSON_GetObjectItem(j, "auto"));
            cJSON* m = cJSON_GetObjectItem(j, "mode");
            parseMode(cJSON_IsString(m) ? m->valuestring : nullptr, sPolicy.mode);
            if (cJSON* c = cJSON_GetObjectItem(j, "check_in_s"); cJSON_IsNumber(c)) sPolicy.checkInS = c->valueint;
            if (cJSON* a = cJSON_GetObjectItem(j, "awake_s"); cJSON_IsNumber(a))
                sPolicy.awakeS = std::max(kMinAwakeS, a->valueint);
            sPolicy.hold = cJSON_IsTrue(cJSON_GetObjectItem(j, "hold"));
            if (cJSON* x = cJSON_GetObjectItem(j, "hold_max_s"); cJSON_IsNumber(x)) sPolicy.holdMaxS = x->valueint;
            cJSON_Delete(j);
        }
    }
    nvs_get_i64(h, "checkin", &sCheckInDue);
    nvs_close(h);
}

void noteActivity() { sActivityUs = esp_timer_get_time(); }

static cJSON* ok(cJSON* payload);
static int64_t nowS();

// ── The battery log: one reading per check-in, the last kLogSize, in NVS ("rcpin" "batlog",
// written once a check-in: 96 a day at the default 15 min). For working out how long the
// battery lasts on a schedule: device.battery_log. ──

struct LogEntry {
    uint32_t t;      // epoch s
    uint16_t mv;     // battery, as the board reads it at the end of the check-in (Wi-Fi up)
    int8_t pct;      // the board's level, -1 none
    uint8_t flags;   // 1 charging, 2 on USB, 4 connected to Muse
};
static constexpr int kLogSize = 256;   // 64 h at 15 min
struct BatteryLog {
    uint16_t next, count;
    LogEntry e[kLogSize];
};

static bool readLog(nvs_handle_t h, BatteryLog& log) {
    size_t n = sizeof log;
    if (nvs_get_blob(h, "batlog", &log, &n) == ESP_OK && n == sizeof log) return true;
    log = {};
    return false;
}

static void logBattery(bool connected) {
    const board::Power pw = rcstage::deviceState().power;
    if (!pw.valid) return;
    nvs_handle_t h;
    if (nvs_open("rcpin", NVS_READWRITE, &h) != ESP_OK) return;
    auto* log = new BatteryLog;
    readLog(h, *log);
    LogEntry& e = log->e[log->next % kLogSize];
    e.t = (uint32_t) nowS();
    e.mv = (uint16_t) std::lround(pw.batteryVolts * 1000);
    e.pct = (int8_t) pw.batteryLevel;
    e.flags = (pw.charging ? 1 : 0) | (pw.powerState >= 1 ? 2 : 0) | (connected ? 4 : 0);
    log->next = (uint16_t) ((log->next + 1) % kLogSize);
    log->count = (uint16_t) std::min(kLogSize, log->count + 1);
    nvs_set_blob(h, "batlog", log, sizeof *log);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "battery %u mV, %d %%%s", e.mv, e.pct, (e.flags & 2) ? " (on USB)" : "");
    delete log;
}

static int64_t nowS() { return (int64_t) time(nullptr); }

static bool onUsb() {
    const board::Power pw = rcstage::deviceState().power;
    return pw.valid && pw.powerState >= 1;
}

// How long to sleep now: until the document's next frame or the next check-in, whichever
// is sooner, less the wake-up cost. 0: stay awake (continuous, or too soon to bother).
static int sleepSeconds(const Policy& p, int64_t checkInDue) {
    const int next = rcstage::nextFrameMs();
    if (next == 0) return 0;   // continuous animation: sleeping would stop it
    int64_t s = std::max<int64_t>(0, checkInDue - nowS());
    if (next > 0) s = std::min<int64_t>(s, next / 1000);
    if (p.mode != board::Sleep::Light) {
        if (s < kMinSleepS) return 0;
        s -= kWakeCostS;
    } else if (s < 2) {
        return 0;
    }
    return (int) s;
}

// Early init on a timed wake, with auto on and no check-in due: the document is loaded and
// drawing; let it draw its frame (and anything due within a few seconds), then sleep again.
// Returns only if the board should boot fully (a check-in is due, or the document animates).
void wakeLocally() {
    Policy p;
    int64_t due;
    {
        std::lock_guard<std::mutex> g(sLock);
        p = sPolicy;
        due = sCheckInDue;
    }
    if (!p.on || p.mode == board::Sleep::Light || nowS() >= due) return;
    const int64_t start = esp_timer_get_time();
    for (int round = 0; round < 10; round++) {
        // The device sampler's values (battery, temperature) arrive within a second or so;
        // they redraw what reads them.
        while (rcstage::framesDrawn() == 0 || esp_timer_get_time() - start < 1500000) vTaskDelay(pdMS_TO_TICKS(50));
        const int next = rcstage::nextFrameMs();
        if (next == 0 || onUsb()) return;
        if (next > 0 && next < (kMinSleepS + 2) * 1000) {   // just short of a boundary: wait for it
            vTaskDelay(pdMS_TO_TICKS(next + 300));
            continue;
        }
        const int s = sleepSeconds(p, due);
        if (s <= 0) return;
        ESP_LOGI(TAG, "local wake done in %lld ms; sleeping %d s", (esp_timer_get_time() - start) / 1000, s);
        board::sleepFor(s, p.mode);
        return;
    }
}

// Awake (a full boot, or after a Light sleep): once the agent has had its time, sleep.
static void autoTask(void*) {
    int64_t connectedAt = 0, wokeAt = esp_timer_get_time();
    bool checkedIn = false;
    const char* woke = board::wakeReason();
    int64_t manualUntil = strcmp(woke, "timer") && strcmp(woke, "rtc") ? wokeAt + (int64_t) kManualAwakeS * 1000000 : 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        Policy p;
        int64_t due;
        {
            std::lock_guard<std::mutex> g(sLock);
            p = sPolicy;
            due = sCheckInDue;
        }
        const bool resumed = sResume.exchange(false);
        if (!p.on) {
            checkedIn = false; connectedAt = 0; wokeAt = esp_timer_get_time();
            sHolding = false;
            continue;
        }
        const int64_t now = esp_timer_get_time();
        if (resumed) manualUntil = 0;
        if (!checkedIn) {
            // A check-in: connected to Muse, then awake_s for the agent (each command, touch
            // or button press gives it awake_s more), or with hold, until the agent resumes.
            // Without a connection in a minute, give up.
            if (museConnected() && !connectedAt) connectedAt = now;
            const int64_t awake = (int64_t) p.awakeS * 1000000;
            bool done;
            if (resumed) done = true;
            else if (connectedAt && p.hold) done = p.holdMaxS > 0 && now >= connectedAt + (int64_t) p.holdMaxS * 1000000;
            else done = now >= (connectedAt ? std::max(connectedAt, sActivityUs.load()) + awake
                                            : wokeAt + awake + 60000000);
            sHolding = connectedAt && p.hold && !done;
            if (!done) continue;
            checkedIn = true;
            std::lock_guard<std::mutex> g(sLock);
            sCheckInDue = nowS() + sPolicy.checkInS;
            due = sCheckInDue;
            save();
            ESP_LOGI(TAG, "checked in (%s%s); next in %d s", connectedAt ? "connected" : "not connected",
                     resumed ? ", resumed" : "", p.checkInS);
            logBattery(connectedAt != 0);
        }
        if (now - sActivityUs.load() < (int64_t) p.awakeS * 1000000) continue;   // someone's using it
        if (now < manualUntil || onUsb()) continue;
        const int s = sleepSeconds(p, due);
        if (s <= 0) continue;
        ESP_LOGI(TAG, "sleeping %d s (%s); document's next frame in %d ms", s, modeName(p.mode), rcstage::nextFrameMs());
        board::sleepFor(s, p.mode);   // Deep and Off don't come back here
        // Light: everything resumes. A check-in when due.
        if (nowS() >= due) { checkedIn = false; connectedAt = 0; wokeAt = esp_timer_get_time(); }
    }
}

void sleepStart() {
    static bool started = false;
    if (started) return;
    started = true;
    xTaskCreate(autoTask, "rc_sleep", 4096, nullptr, 2, nullptr);
}

// ── device.sleep ──

static cJSON* ok(cJSON* payload) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", payload);
    return r;
}

struct Once { int seconds; board::Sleep mode; };

static void onceTask(void* arg) {
    auto* o = static_cast<Once*>(arg);
    vTaskDelay(pdMS_TO_TICKS(1500));   // let the reply reach Muse
    board::sleepFor(o->seconds, o->mode);
    delete o;
    vTaskDelete(nullptr);
}

cJSON* sleepCommand(cJSON* params) {
    board::Sleep mode;
    cJSON* m = cJSON_GetObjectItem(params, "mode");
    if (!parseMode(cJSON_IsString(m) ? m->valuestring : nullptr, mode))
        return gadget_ext_error("bad_param", "mode: off, deep or light");
    cJSON* a = cJSON_GetObjectItem(params, "auto");
    auto num = [&](const char* k, int otherwise) {
        cJSON* v = cJSON_GetObjectItem(params, k);
        return cJSON_IsNumber(v) ? v->valueint : (cJSON_IsString(v) ? atoi(v->valuestring) : otherwise);
    };
    cJSON* p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "next_frame_ms", rcstage::nextFrameMs());
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "resume"))) {
        // End the check-in (or the window after a button press) and go back to the cycle:
        // asleep about 2 s from now, once this reply is on its way.
        std::lock_guard<std::mutex> g(sLock);
        if (!sPolicy.on) { cJSON_Delete(p); return gadget_ext_error("bad_param", "auto sleep is off"); }
        sActivityUs = esp_timer_get_time() - (int64_t) sPolicy.awakeS * 1000000 + 2000000;
        sResume = true;
        cJSON_AddBoolToObject(p, "resumed", true);
        cJSON_AddBoolToObject(p, "on_usb", onUsb());   // on USB it stays awake anyway
        cJSON_AddNumberToObject(p, "next_check_in", (double) (nowS() + sPolicy.checkInS));
        return ok(p);
    }
    if (a) {
        std::lock_guard<std::mutex> g(sLock);
        sPolicy.on = cJSON_IsTrue(a) || (cJSON_IsNumber(a) && a->valueint) ||
                     (cJSON_IsString(a) && !strcmp(a->valuestring, "true"));
        if (m) sPolicy.mode = mode;
        sPolicy.checkInS = std::clamp(num("check_in_s", sPolicy.checkInS), 60, 255 * 60);
        sPolicy.awakeS = std::clamp(num("awake_s", sPolicy.awakeS), kMinAwakeS, 600);
        if (cJSON* h = cJSON_GetObjectItem(params, "hold")) sPolicy.hold = cJSON_IsTrue(h);
        const int maxS = num("hold_max_s", sPolicy.holdMaxS);
        sPolicy.holdMaxS = maxS <= 0 ? 0 : std::clamp(maxS, 60, 24 * 3600);
        sCheckInDue = nowS() + sPolicy.checkInS;   // this session counts as a check-in
        save();
        cJSON* o = cJSON_AddObjectToObject(p, "auto");
        cJSON_AddBoolToObject(o, "on", sPolicy.on);
        cJSON_AddStringToObject(o, "mode", modeName(sPolicy.mode));
        cJSON_AddNumberToObject(o, "check_in_s", sPolicy.checkInS);
        cJSON_AddNumberToObject(o, "awake_s", sPolicy.awakeS);
        cJSON_AddBoolToObject(o, "hold", sPolicy.hold);
        cJSON_AddNumberToObject(o, "hold_max_s", sPolicy.holdMaxS);
        noteActivity();   // awake_s from now before the first sleep
        return ok(p);
    }
    int seconds = num("seconds", 0);
    if (seconds <= 0) {
        // Until the document's next frame.
        const int next = rcstage::nextFrameMs();
        if (next == 0) { cJSON_Delete(p); return gadget_ext_error("bad_param", "the document animates continuously; give seconds"); }
        if (next < 0) { cJSON_Delete(p); return gadget_ext_error("bad_param", "the document has nothing scheduled; give seconds"); }
        seconds = std::max(1, next / 1000);
    }
    seconds = std::clamp(seconds, 1, mode == board::Sleep::Off ? 255 * 60 : 24 * 3600);
    cJSON_AddStringToObject(p, "mode", modeName(mode));
    cJSON_AddNumberToObject(p, "seconds", seconds);
    auto* o = new Once{seconds, mode};
    if (xTaskCreate(onceTask, "rc_sleep1", 4096, o, 3, nullptr) != pdPASS) {
        delete o;
        cJSON_Delete(p);
        return gadget_ext_error("out_of_memory", "could not start");
    }
    return ok(p);
}

// device.battery_log {clear?: bool} -> the readings, oldest first, and the drain over the
// latest stretch on battery.
cJSON* batteryLogCommand(cJSON* params) {
    nvs_handle_t h;
    if (nvs_open("rcpin", NVS_READWRITE, &h) != ESP_OK) return gadget_ext_error("unavailable", "NVS unavailable");
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "clear"))) {
        nvs_erase_key(h, "batlog");
        nvs_commit(h);
        nvs_close(h);
        return ok(cJSON_CreateObject());
    }
    auto* log = new BatteryLog;
    readLog(h, *log);
    nvs_close(h);
    cJSON* p = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(p, "readings");
    const int first = (log->next - log->count + kLogSize) % kLogSize;
    int runStart = -1, runEnd = -1;   // the latest stretch of readings on battery
    for (int i = 0; i < log->count; i++) {
        const LogEntry& e = log->e[(first + i) % kLogSize];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", e.t);
        cJSON_AddNumberToObject(o, "mv", e.mv);
        cJSON_AddNumberToObject(o, "pct", e.pct);
        cJSON_AddBoolToObject(o, "usb", e.flags & 2);
        cJSON_AddBoolToObject(o, "charging", e.flags & 1);
        cJSON_AddBoolToObject(o, "connected", e.flags & 4);
        cJSON_AddItemToArray(list, o);
        if (e.flags & 2) runStart = runEnd = -1;
        else { if (runStart < 0) runStart = i; runEnd = i; }
    }
    if (runStart >= 0 && runEnd > runStart) {
        const LogEntry& a = log->e[(first + runStart) % kLogSize];
        const LogEntry& b = log->e[(first + runEnd) % kLogSize];
        const double hours = (b.t - a.t) / 3600.0;
        cJSON* d = cJSON_AddObjectToObject(p, "on_battery");
        cJSON_AddNumberToObject(d, "readings", runEnd - runStart + 1);
        cJSON_AddNumberToObject(d, "hours", std::round(hours * 100) / 100);
        cJSON_AddNumberToObject(d, "mv_drop", (double) a.mv - b.mv);
        if (hours > 0) cJSON_AddNumberToObject(d, "mv_per_hour", std::round((a.mv - b.mv) / hours * 10) / 10);
        if (hours > 0 && a.pct >= 0 && b.pct >= 0 && a.pct > b.pct) {
            // Rough: the board's level is a straight line from 3.30 to 4.15 V, which a
            // lithium cell isn't; trust it over a day or more, not an hour.
            const double pctPerHour = (a.pct - b.pct) / hours;
            cJSON_AddNumberToObject(d, "pct_per_hour", std::round(pctPerHour * 100) / 100);
            cJSON_AddNumberToObject(d, "days_full_to_empty_rough", std::round(100 / pctPerHour / 24 * 10) / 10);
        }
    }
    delete log;
    return ok(p);
}

void sleepInfo(cJSON* parent) {
    std::lock_guard<std::mutex> g(sLock);
    cJSON* o = cJSON_AddObjectToObject(parent, "sleep");
    cJSON_AddStringToObject(o, "woke", board::wakeReason());
    cJSON_AddBoolToObject(o, "auto", sPolicy.on);
    cJSON_AddStringToObject(o, "mode", modeName(sPolicy.mode));
    cJSON_AddNumberToObject(o, "check_in_s", sPolicy.checkInS);
    cJSON_AddNumberToObject(o, "awake_s", sPolicy.awakeS);
    cJSON_AddBoolToObject(o, "hold", sPolicy.hold);
    cJSON_AddNumberToObject(o, "hold_max_s", sPolicy.holdMaxS);
    cJSON_AddBoolToObject(o, "holding", sHolding.load());
    if (sPolicy.on) cJSON_AddNumberToObject(o, "next_check_in", (double) sCheckInDue);
    cJSON_AddNumberToObject(o, "next_frame_ms", rcstage::nextFrameMs());
}

}  // namespace muserc
