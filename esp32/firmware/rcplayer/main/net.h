// Wi-Fi (on-chip, or through the Tab5's ESP32-C6 co-processor) + a kobento-net receiver.
//
// kobento-net protocol v1 (formerly rcNet / Origami2; the wire is unchanged, so existing
// senders keep working):
//   mDNS  _kobento._tcp  TXT v=1, name=<device>   (also _rcnet._tcp, for legacy discovery)
//   GET  /info   -> {"name": "...", "version": 1}
//   POST /send   headers X-Filename, X-Filesize; body = the file -> {"status":"ok",...}
//   POST /named  "name=value" lines that set the documents' named variables (how Home
//                Assistant pushes to the display, docs/RC_SYSTEM_VARIABLES.md).
// See the kobento-net repo (PROTOCOL.md) for the full spec.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {

void start();                                             // non-blocking; connects in background

// Saved networks (NVS, up to 8; an empty password is an open network). The player joins
// the last one used when it is in range, else the strongest saved one, and falls back to the
// others when one fails.
bool addNetwork(const std::string& ssid, const std::string& pass);   // save; don't switch
bool setCredentials(const std::string& ssid, const std::string& pass);   // save and join
bool forget(const std::string& ssid);
// Switch to a saved network, or to an open one seen in the last scan (remembered once
// joined). Returns at once; the Wi-Fi task does the work.
void join(const std::string& ssid);
void scan(bool print = false);                            // in the background
std::string status();                                     // state and saved networks, for the console
bool connected();                                         // has an IP address

// What the Wi-Fi screen shows. state: 1 connected, 2 connecting, 3 saved, 4 open (not
// saved), 5 saved but not in range, 6 couldn't connect, 7 needs a password (not saved).
// signal 0..100.
struct Entry { std::string ssid; int state; int signal; bool lock; };
struct View { std::string status; bool scanning = false; std::vector<Entry> entries; };
View view();
// Called from the Wi-Fi task whenever the view may have changed.
void onChange(void (*cb)());

struct Received {
    std::string name;
    std::vector<uint8_t> bytes;
};
bool pollReceived(Received& out);                         // non-blocking, player loop

}  // namespace net
