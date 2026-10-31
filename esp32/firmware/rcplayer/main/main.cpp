// RemoteCompose player.
//
// rcstage shows the documents; this app adds the embedded set, the serial console and the
// rcNet receiver. A long press (1.5 s, no movement) switches to the next embedded document.
//
// Documents drive the device with hostAction(0, "name[=value]") (docs/RC_SYSTEM_VARIABLES.md):
//   system.backlight=<0..1>|cycle   doc.show=<name>   doc.next   doc.back
//   wifi.join=<row>   wifi.scan   wifi.page            (the Wi-Fi screen, wifi_settings)
//
// The Control.* named variables (docs/RC_SYSTEM_VARIABLES.md) go both ways. A document that
// declares Control.next / Control.back gets the board's buttons as press counts, and they no
// longer step the documents. And the player listens to the class (what a touch changed, once
// the finger lifts): a document that changes
// Control.climate.<id>.<temperature | target_temp_low | target_temp_high> asks Home Assistant
// to set it; once sent, the player puts it back to 0 (nothing asked). Control.dial.climate.<id>
// (a dial dragged to a temperature) asks the same for temperature, and the player holds it
// in Control.climate.<id>.temperature until it's sent, so the page keeps showing it.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "rcstage/stage.h"
#include "board/board.h"
#include "rcstage/device_state.h"

#include "console.h"
#include "net.h"
#include "ha.h"
#include "embedded_docs.h"

static const char* TAG = "rcplayer";

static constexpr int kDocs = sizeof(kEmbeddedDocs) / sizeof(kEmbeddedDocs[0]);
static int sCurrent = 0, sPrevious = 0;
static constexpr const char* kWifiDoc = "wifi_settings";

static int findExact(const std::string& name);
static bool otherScreen(int i);
static bool isWifiDoc(const char* name) { return !strncmp(name, kWifiDoc, strlen(kWifiDoc)); }

// Player task: open embedded document i, skipping any that fail. `step` (+1 or -1) is the
// direction to skip in when i is a variant for another screen; 0 opens i as asked.
static void open(int i, int step = 0) {
    const int from = sCurrent;
    for (int tries = 0; tries < kDocs; tries++, i += step ? step : 1) {
        const int at = ((i % kDocs) + kDocs) % kDocs;
        if (step && otherScreen(at)) continue;
        sCurrent = at;
        const auto& d = kEmbeddedDocs[sCurrent];
        if (rcstage::load(d.start, d.end - d.start, d.name, d.fit)) {
            if (sCurrent != from) sPrevious = from;
            if (isWifiDoc(d.name)) net::scan();   // fresh list on the way in
            return;
        }
    }
}

static int findExact(const std::string& name) {
    for (int i = 0; i < kDocs; i++)
        if (name == kEmbeddedDocs[i].name) return i;
    return -1;
}

// A document may come in variants for particular screens, shown wherever <name> is asked
// for: "<name>_round" on a round screen, "<name>_light" (a white palette) on e-ink.
struct Variant { const char* suffix; bool mine; };
static constexpr Variant kVariants[] = {{"_round", board::kRound}, {"_light", board::kEink}};

static int findDoc(const std::string& name) {
    for (const Variant& v : kVariants) {
        if (!v.mine) continue;
        int i = findExact(name + v.suffix);
        if (i >= 0) return i;
    }
    return findExact(name);
}

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
    return findDoc(n) != i;
}

// ── The Wi-Fi screen ─────────────────────────────────────────────────────────────────────
//
// The network list goes to the document as named variables, a page of rows at a time:
// as many 37 dp rows as fit under the 44 dp header, up to the document's 10.

static constexpr int kSlots = 10;
static std::mutex sWifiLock;
static std::vector<std::string> sRows;   // the SSIDs on screen, by row
static int sPage = 0;
// A network picked on the screen: once it is joined, go back to the screen before.
static std::string sJoining;
static int64_t sReturnAt = 0;      // us; player task
static std::atomic<bool> sJoined{false};

// A 44 dp header, then 37 dp rows. wifi_settings_light (e-ink) draws them 1.5x larger, with
// the status on a 24 dp line of its own under the header.
static constexpr float kWifiHeader = board::kEink ? 44 * 1.5f + 24 : 44;
static constexpr float kWifiRow = board::kEink ? 37 * 1.5f : 37;

static int rowsThatFit() {
    if (board::kRound) return 3;   // wifi_settings_round: three rows across the circle
    const float dpHeight = board::kHeight / board::kDensity;
    return std::max(1, std::min(kSlots, (int) ((dpHeight - kWifiHeader) / kWifiRow)));
}

// Long names end in "…", cut on a UTF-8 boundary. Round rows are narrower, and the larger
// light rows hold fewer characters.
static std::string shorten(const std::string& s,
                           size_t max = board::kRound ? 17 : board::kEink ? 20 : 26) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && ((uint8_t) s[n] & 0xC0) == 0x80) n--;
    return s.substr(0, n) + "…";
}

// Any task (the Wi-Fi task, or the player task after a page turn).
static void publishWifi() {
    const net::View v = net::view();
    {
        std::lock_guard<std::mutex> g(sWifiLock);
        for (auto& e : v.entries)
            if (!sJoining.empty() && e.ssid == sJoining && e.state == 1) { sJoining.clear(); sJoined = true; }
    }
    const int fit = rowsThatFit();
    const int n = (int) v.entries.size();
    const int pages = std::max(1, (n + fit - 1) / fit);
    std::lock_guard<std::mutex> g(sWifiLock);
    sPage = std::min(sPage, pages - 1);
    sRows.assign(kSlots, "");
    rcstage::setNamed("system.wifi_status", v.status);
    // The start-up screen's status line (named variables only: no overlay on other documents).
    rcstage::setNamed("system.banner_title", v.status);
    rcstage::setNamed("system.banner_detail", net::connected()
        ? std::string("rcNet: ") + CONFIG_RCPLAYER_DEVICE_NAME + ".local" : std::string());
    // No Muse here: the status dot (muse.link 0) takes the banner colour, green once online.
    rcstage::setNamed("muse.link", "0");
    rcstage::setNamed("system.banner_color", net::connected() ? "#4CD964" : "#FFC107");
    rcstage::setNamed("system.wifi_scanning", v.scanning ? "1" : "0");
    rcstage::setNamed("system.wifi_pages", std::to_string(pages));
    rcstage::setNamed("system.wifi_page",
                      pages > 1 ? std::to_string(sPage + 1) + "/" + std::to_string(pages) : "");
    for (int r = 0; r < kSlots; r++) {
        const int i = sPage * fit + r;
        const bool on = r < fit && i < n;
        const std::string row = "system.wifi_" + std::to_string(r) + "_";
        if (on) sRows[r] = v.entries[i].ssid;
        rcstage::setNamed(row + "ssid", on ? shorten(v.entries[i].ssid) : "");
        rcstage::setNamed(row + "state", std::to_string(on ? v.entries[i].state : 0));
        rcstage::setNamed(row + "signal", std::to_string(on ? v.entries[i].signal : 0));
        rcstage::setNamed(row + "lock", on && v.entries[i].lock ? "1" : "0");
    }
}

// ── Control.* (player task) ──────────────────────────────────────────────────────────────

static int sNextPresses = 0, sBackPresses = 0;

// The board's buttons: to the document when it takes them, else through the documents.
static void button(int step) {
    if (!rcstage::declaresNamed("Control.next") && !rcstage::declaresNamed("Control.back")) {
        open(sCurrent + step, step);
        return;
    }
    if (step > 0) rcstage::setNamed("Control.next", std::to_string(++sNextPresses));
    else rcstage::setNamed("Control.back", std::to_string(++sBackPresses));
}

// A document changed a Control.* variable.
static void controlChanged(const std::string& name, const std::string& value) {
    rcstage::pushEvent({0, 0, "control", 0, 0, name, value});
    static const std::string kClimate = "Control.climate.", kDial = "Control.dial.climate.";
    if (name.compare(0, kDial.size(), kDial) == 0) {
        const std::string entity = name.substr(strlen("Control.dial."));
        const float v = std::strtof(value.c_str(), nullptr);
        if (v == 0) return;   // another page's dial
        rcstage::setNamed("Control." + entity + ".temperature", value);
        ha::setClimate(entity, "temperature", v);
        return;
    }
    if (name.compare(0, kClimate.size(), kClimate) == 0) {
        // Control.climate.<object_id>.<attr>: the entity is climate.<object_id>.
        const size_t dot = name.rfind('.');
        const std::string entity = name.substr(strlen("Control."), dot - strlen("Control."));
        const float v = std::strtof(value.c_str(), nullptr);
        if (v != 0) ha::setClimate(entity, name.substr(dot + 1), v);   // 0: nothing asked
    }
}

// ── Host actions (player task) ───────────────────────────────────────────────────────────

static int sBacklight = 100;

static void runHostAction(const std::string& name, const std::string& value) {
    if (name == "system.backlight") {
        // A fraction 0..1, or "cycle" through 100 -> 60 -> 30 %.
        int pct = value == "cycle" ? (sBacklight > 60 ? 60 : sBacklight > 30 ? 30 : 100)
                                   : (int) (std::strtof(value.c_str(), nullptr) * 100);
        sBacklight = std::max(5, std::min(100, pct));
        board::setBrightness(sBacklight);
        rcstage::setNamed("system.backlight", std::to_string(sBacklight / 100.0f));
    } else if (name == "doc.show") {
        int i = findDoc(value);
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
        if (!ssid.empty()) {
            {
                std::lock_guard<std::mutex> g(sWifiLock);
                sJoining = ssid;
            }
            net::join(ssid);
        }
    } else if (name == "wifi.scan") {
        net::scan();
    } else if (name == "wifi.page") {
        {
            std::lock_guard<std::mutex> g(sWifiLock);
            sPage++;   // publishWifi wraps it below
            const int fit = rowsThatFit();
            if (sPage * fit >= (int) net::view().entries.size()) sPage = 0;
        }
        publishWifi();
    } else {
        ESP_LOGW(TAG, "unknown host action '%s'", name.c_str());
    }
}

static void tick() {
    console::Request req;
    while (console::poll(req)) {
        switch (req.cmd) {
            case console::Cmd::Snap:
                board::dumpFrame(board::kWidth > 400 ? 2 : 1);   // ~360 px wide
                break;
            case console::Cmd::Res: rcstage::setRenderDiv(req.arg); break;
            case console::Cmd::Next: open(sCurrent + 1, 1); break;
            case console::Cmd::Prev: open(sCurrent - 1, -1); break;
            case console::Cmd::Doc: open(req.arg); break;
            case console::Cmd::List:
                for (int i = 0; i < kDocs; i++)
                    printf("%c %d %s\n", i == sCurrent ? '*' : ' ', i, kEmbeddedDocs[i].name);
                break;
            case console::Cmd::Stats: rcstage::requestStats(); break;
            case console::Cmd::Bench: rcstage::bench(); break;
            case console::Cmd::Act: {
                std::string md = req.text;
                size_t eq = md.find('=');
                printf("act %s\n", md.c_str());
                runHostAction(md.substr(0, eq), eq == std::string::npos ? "" : md.substr(eq + 1));
                break;
            }
            default: break;
        }
    }
    // Hardware buttons: the document's when it declares Control.next / .back, else they step
    // through the documents, forward like a long press, or back.
    if (int step = board::buttonPressed()) button(step);
    // Joined the network picked on the Wi-Fi screen: show "Connected" briefly, then go back.
    if (sJoined.exchange(false)) sReturnAt = esp_timer_get_time() + 1500000;
    if (sReturnAt && esp_timer_get_time() >= sReturnAt) {
        sReturnAt = 0;
        if (isWifiDoc(kEmbeddedDocs[sCurrent].name)) open(sPrevious);
    }
    net::Received rx;
    if (net::pollReceived(rx)) rcstage::show(std::move(rx.bytes), "rcNet: " + rx.name);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "RemoteCompose player on %s", board::kName);
    console::start();
    net::start();
    ha::start();
    if (!board::displayInit()) return;
    rcstage::listenNamed("Control.*", controlChanged);
    ha::onSetDone([](const std::string& entity, const std::string& attr) {
        rcstage::setNamed("Control." + entity + "." + attr, "0");   // sent: the document shows HA's value again
    });
    rcstage::Hooks hooks;
    hooks.tick = tick;
    hooks.longPress = [] { open(sCurrent + 1, 1); };
    hooks.hostAction = [](int id, const std::string& md) {
        if (id != 0) return;
        size_t eq = md.find('=');
        runHostAction(md.substr(0, eq), eq == std::string::npos ? "" : md.substr(eq + 1));
    };
    hooks.namedAction = [](const std::string& name, int, const std::string& value) {
        runHostAction(name, value);
    };
    // Start on the start-up screen when it is built in.
    sCurrent = sPrevious = std::max(0, findDoc("muse_splash"));
    const auto& d = kEmbeddedDocs[sCurrent];
    rcstage::start(hooks, d.start, d.end - d.start, d.name);
    rcstage::deviceStateStart();
    rcstage::setNamed("system.muse", "0");   // no Muse here: the start screen shows RC alone
    net::onChange(publishWifi);
    publishWifi();
}
