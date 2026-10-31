#include "console.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "board/board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ble.h"
#include "ha.h"
#include "net.h"
#include "rcstage/stage.h"
#include <string>
#include <vector>

namespace console {

static QueueHandle_t sQueue;

// wifi add SSID [PASSWORD] | join SSID | forget SSID | list | SSID [PASSWORD]
// The SSID has no spaces; the rest of the line is the password.
static void wifiCommand(const std::string& args) {
    size_t sp = args.find(' ');
    std::string verb = args.substr(0, sp), rest = sp == std::string::npos ? "" : args.substr(sp + 1);
    auto split = [](const std::string& s, std::string& ssid, std::string& pass) {
        size_t p = s.find(' ');
        ssid = s.substr(0, p);
        pass = p == std::string::npos ? "" : s.substr(p + 1);
    };
    std::string ssid, pass;
    if (verb == "add") {
        split(rest, ssid, pass);
        printf("%s\n", net::addNetwork(ssid, pass) ? "wifi: saved" : "wifi: save failed");
    } else if (verb == "join") {
        net::join(rest);
        printf("wifi: joining '%s'\n", rest.c_str());
    } else if (verb == "forget") {
        printf("%s\n", net::forget(rest) ? "wifi: forgotten" : "wifi: not saved");
    } else if (verb == "list") {
        printf("%s\n", net::status().c_str());
    } else {
        split(args, ssid, pass);
        printf("%s\n", net::setCredentials(ssid, pass) ? "wifi: saved, joining" : "wifi: save failed");
    }
}

static void readerTask(void*) {
    char line[512];   // room for "ha token <a long-lived HA token>"
    size_t n = 0;
    while (true) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < sizeof(line) - 1) line[n++] = (char) c;
            continue;
        }
        line[n] = 0;
        n = 0;
        Request r;
        if (!strcmp(line, "snap")) r.cmd = Cmd::Snap;
        else if (!strcmp(line, "next")) r.cmd = Cmd::Next;
        else if (!strcmp(line, "prev")) r.cmd = Cmd::Prev;
        else if (!strcmp(line, "list")) r.cmd = Cmd::List;
        else if (!strcmp(line, "stats")) r.cmd = Cmd::Stats;
        else if (!strcmp(line, "bench")) r.cmd = Cmd::Bench;
        else if (!strncmp(line, "wifi ", 5)) { wifiCommand(line + 5); continue; }
        else if (!strcmp(line, "ha") || !strncmp(line, "ha ", 3)) {
            if (!ha::console(line[2] ? line + 3 : "")) printf("? ha [url URL | token TOKEN | entities ID ... | poll]\n");
            continue;
        }
        else if (!strcmp(line, "named") || !strncmp(line, "named ", 6)) {
            // The showing document's named variables and their values, those starting with
            // the prefix given.
            std::vector<rcstage::NamedValue> vs;
            const char* prefix = line[5] ? line + 6 : "";
            if (!rcstage::namedValues(vs, 2000)) printf("named: no answer from the player\n");
            for (auto& v : vs)
                if (!strncmp(v.name.c_str(), prefix, strlen(prefix)))
                    printf("%s = %s (%s)\n", v.name.c_str(), v.value.c_str(), v.type.c_str());
            continue;
        }
        else if (!strcmp(line, "net")) { printf("%s\n", net::status().c_str()); continue; }
        else if (!strcmp(line, "scan")) { net::scan(true); continue; }
        else if (!strcmp(line, "ble")) { ble::start(); continue; }
        else if (!strncmp(line, "act ", 4)) {
            r.cmd = Cmd::Act;
            strlcpy(r.text, line + 4, sizeof(r.text));
        }
        else if (!strncmp(line, "doc ", 4)) { r.cmd = Cmd::Doc; r.arg = atoi(line + 4); }
        else if (!strcmp(line, "res full")) { r.cmd = Cmd::Res; r.arg = 1; }
        else if (!strcmp(line, "res half")) { r.cmd = Cmd::Res; r.arg = 2; }
        else if (board::console(line)) continue;   // board diagnostics: "pmu", "expander", ...
        else if (line[0]) { printf("? unknown command '%s'\n", line); continue; }
        xQueueSend(sQueue, &r, 0);
    }
}

void start() {
    sQueue = xQueueCreate(8, sizeof(Request));
    board::consoleInit();   // blocking, driver-backed stdin/stdout on the board's console port
    xTaskCreatePinnedToCore(readerTask, "console", 4096, nullptr, 3, nullptr, 0);
}

bool poll(Request& out) { return xQueueReceive(sQueue, &out, 0) == pdTRUE; }

}  // namespace console
