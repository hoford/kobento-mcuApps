// Between muse_rc.cpp (the screen, Wi-Fi, status) and commands.cpp (the Muse commands).
#pragma once
#include <string>
#include <vector>

#include "cJSON.h"
#include "noise_control.h"
#include "embedded_docs.h"

namespace muserc {

// muse_rc.cpp
const EmbeddedDoc* findDoc(const char* name);        // with this screen's variant (_round, _light)
const EmbeddedDoc* findDocExact(const char* name);   // that name only
bool stageUp();                                      // the display and rcstage started
bool museConnected();                                // the agent session is up

// commands.cpp
struct Pinned {
    std::vector<uint8_t> bytes;
    std::string name;
    bool fit = false;
};
// The document rc.pin kept, to show at boot (and the persisted variable values, applied).
// NotYet until the SDK has initialised NVS: call again later.
enum class PinLoad { NotYet, None, Loaded };
PinLoad loadPinned(Pinned& out);
void registerCommands(cJSON* commands);
cJSON* command(const char* command, cJSON* params, const char* requestId, noise_ctrl_session_generation_t gen);
void health(cJSON* health);
bool consoleApi(const std::string& line);   // "api <command> [json]"

// sleep.cpp (device.sleep)
void sleepLoad();       // the policy, once NVS is up
void sleepStart();      // the auto-sleep task
void wakeLocally();     // early init, timed wake: draw and sleep again, or return to boot
void noteActivity();    // a command, touch or button: stay awake a while
cJSON* sleepCommand(cJSON* params);
cJSON* batteryLogCommand(cJSON* params);
void sleepInfo(cJSON* parent);

// muse_rc.cpp (device.wifi). Callers need an internal-RAM stack (NVS).
cJSON* wifiCommand(cJSON* params);
std::string wifiSsid();   // the network joined, "" if none

// commands.cpp: the pinned document's name, "" if none (from memory: any task).
std::string pinnedNow();

}  // namespace muserc
