// The Muse commands (link.register / link.invoke) for a board with a RemoteCompose screen.
// docs/MUSE_COMMANDS.md documents each one; keep the two in step.
//
//   rc.*        what the screen shows: documents, their variables, pinning one across
//               reboots, a screenshot, the screen's geometry, touches and other events
//   device.*    time (system clock, RTC, NTP), beep, motion; extras for device.health
//   display.*   refresh (e-ink: clear ghosting)
//
// Commands run on the Noise session's task and must not block: slow ones (downloads, flash
// writes, screenshots) reply later from a task of their own. The serial console's
// "api <command> [json]" runs any of them locally and prints the result (tools/muse_api.py).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "miniz.h"   // the ROM's deflate (tdefl)
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "gadget_ext.h"
#include "board/board.h"
#include "rcstage/device_state.h"
#include "rcstage/stage.h"
#include "muse_rc_internal.h"

static const char* TAG = "muse_api";

namespace muserc {

static constexpr size_t kMaxDoc = 4 * 1024 * 1024;
static constexpr const char* kConsoleRequest = "console";

// ── Replies ─────────────────────────────────────────────────────────────────────────────

static cJSON* ok(cJSON* payload = nullptr) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(r, "payload", payload ? payload : cJSON_CreateObject());
    return r;
}

static cJSON* async() {
    cJSON* a = cJSON_CreateObject();
    cJSON_AddBoolToObject(a, "_async", true);
    return a;
}

// A console request prints its result; the base64 of an image goes between PNG BEGIN and
// PNG END lines, for tools/muse_api.py to save.
static void printResult(cJSON* result) {
    cJSON* payload = cJSON_GetObjectItem(result, "payload");
    cJSON* data = payload ? cJSON_DetachItemFromObject(payload, "data_base64") : nullptr;
    if (cJSON_IsString(data)) {
        const char* s = data->valuestring;
        const size_t n = strlen(s);
        printf("\nPNG BEGIN %u\n", (unsigned) n);
        for (size_t i = 0; i < n; i += 76) printf("%.*s\n", (int) std::min<size_t>(76, n - i), s + i);
        printf("PNG END\n");
        cJSON_AddNumberToObject(payload, "data_base64_chars", (double) n);
    }
    cJSON_Delete(data);
    char* json = cJSON_PrintUnformatted(result);
    printf("API RESULT %s\n", json ? json : "?");
    cJSON_free(json);
    fflush(stdout);
}

struct Request {
    std::string id;
    noise_ctrl_session_generation_t generation;
};

static void reply(const Request& r, cJSON* result) {
    if (r.id == kConsoleRequest) {
        printResult(result);
        cJSON_Delete(result);
    } else {
        noise_ctrl_send_command_result(r.generation, r.id.c_str(), result);
    }
}

// ── Parameters ──────────────────────────────────────────────────────────────────────────

struct Param { const char* name; const char* type; const char* description; };

static cJSON* params(std::initializer_list<Param> ps) {
    if (ps.size() == 0) return nullptr;
    cJSON* o = cJSON_CreateObject();
    for (const Param& p : ps) {
        cJSON* f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "type", p.type);
        cJSON_AddStringToObject(f, "description", p.description);
        cJSON_AddItemToObject(o, p.name, f);
    }
    return o;
}

static const char* str(cJSON* params, const char* key) {
    cJSON* v = cJSON_GetObjectItem(params, key);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

// A boolean sent as true/false, 1/0 or "true"/"false".
static bool flag(cJSON* params, const char* key, bool otherwise = false) {
    cJSON* v = cJSON_GetObjectItem(params, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return !strcmp(v->valuestring, "true") || !strcmp(v->valuestring, "1");
    return otherwise;
}

static double number(cJSON* params, const char* key, double otherwise) {
    cJSON* v = cJSON_GetObjectItem(params, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && *v->valuestring) return strtod(v->valuestring, nullptr);
    return otherwise;
}

// A variable's value as rcstage takes it: text as is, numbers without a trailing ".0".
static bool valueText(cJSON* v, std::string& out) {
    char b[32];
    if (cJSON_IsString(v)) out = v->valuestring;
    else if (cJSON_IsNumber(v)) { snprintf(b, sizeof b, "%.9g", v->valuedouble); out = b; }
    else if (cJSON_IsBool(v)) out = cJSON_IsTrue(v) ? "1" : "0";
    else return false;
    return true;
}

static double ageMs(int64_t atUs) { return atUs ? (esp_timer_get_time() - atUs) / 1000.0 : -1; }

// ── Persistence ─────────────────────────────────────────────────────────────────────────
//
// rc.pin keeps a document across reboots. A built-in one is kept by name (NVS); any other
// (downloaded) is kept whole in flash: a region above the partition table, which ends at
// 8 MB on these 16 MB boards, registered at run time (esp_partition_register_external), so
// the partition table, and with it the app-only update, stays as it is. Variables set with
// persist come back at boot too (NVS, a JSON object).

static constexpr size_t kPinOffset = 0xC00000, kPinSize = 0x400000;   // 12..16 MB
static constexpr uint32_t kPinMagic = 0x4E504352;   // "RCPN"

struct PinHeader {
    uint32_t magic, version, size, crc;
    uint8_t fit, pad[3];
    char name[64];
    char url[256];
};

static const esp_partition_t* pinRegion() {
    static const esp_partition_t* part;
    static bool tried;
    if (tried) return part;
    tried = true;
    uint32_t flashSize = 0;
    if (esp_flash_get_size(esp_flash_default_chip, &flashSize) != ESP_OK || flashSize < kPinOffset + kPinSize) {
        ESP_LOGW(TAG, "pin: flash is %u bytes, too small for the pin region", (unsigned) flashSize);
        return nullptr;
    }
    esp_err_t e = esp_partition_register_external(esp_flash_default_chip, kPinOffset, kPinSize, "rcpin",
                                                  ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t) 0x99, &part);
    if (e != ESP_OK) {
        // It overlaps a partition: a newer partition table uses this space.
        ESP_LOGW(TAG, "pin: no region (%s)", esp_err_to_name(e));
        part = nullptr;
    }
    return part;
}

static std::mutex sPinLock;   // the flash region, NVS and these:
static std::string sPinnedName;   // "" = nothing pinned
static cJSON* sKeptValues;        // persisted variables {name: value}

static bool nvsOpen(nvs_handle_t* h, bool write) {
    return nvs_open("rcpin", write ? NVS_READWRITE : NVS_READONLY, h) == ESP_OK;
}

static std::string nvsString(nvs_handle_t h, const char* key) {
    size_t n = 0;
    if (nvs_get_str(h, key, nullptr, &n) != ESP_OK || n == 0) return {};
    std::string s(n, '\0');
    if (nvs_get_str(h, key, s.data(), &n) != ESP_OK) return {};
    s.resize(n - 1);
    return s;
}

// What to show once NVS is up. NVS isn't initialised here: the SDK does that after early
// init, and doing it sooner takes internal RAM the Tab5's SDIO radio link needs at boot.
PinLoad loadPinned(Pinned& out) {
    std::lock_guard<std::mutex> g(sPinLock);
    nvs_handle_t h;
    const esp_err_t e = nvs_open("rcpin", NVS_READONLY, &h);
    if (e == ESP_ERR_NVS_NOT_INITIALIZED) return PinLoad::NotYet;
    if (e != ESP_OK) return PinLoad::None;   // ESP_ERR_NVS_NOT_FOUND: never pinned
    const std::string kind = nvsString(h, "kind"), name = nvsString(h, "name"), values = nvsString(h, "values");
    nvs_close(h);
    if (!values.empty()) {
        sKeptValues = cJSON_Parse(values.c_str());
        cJSON* v;
        cJSON_ArrayForEach(v, sKeptValues) {
            std::string text;
            if (valueText(v, text)) rcstage::setNamed(v->string, text);
        }
    }
    if (kind == "builtin") {
        const EmbeddedDoc* d = findDocExact(name.c_str());
        if (!d) return PinLoad::None;
        out.bytes.assign(d->start, d->end);
        out.name = d->name;
        out.fit = d->fit;
    } else if (kind == "flash") {
        const esp_partition_t* p = pinRegion();
        PinHeader hd;
        if (!p || esp_partition_read(p, 0, &hd, sizeof hd) != ESP_OK || hd.magic != kPinMagic ||
            hd.size == 0 || hd.size > kPinSize - 4096)
            return PinLoad::None;
        out.bytes.resize(hd.size);
        if (esp_partition_read(p, 4096, out.bytes.data(), hd.size) != ESP_OK ||
            esp_rom_crc32_le(0, out.bytes.data(), hd.size) != hd.crc) {
            ESP_LOGW(TAG, "pin: the saved document is damaged");
            return PinLoad::None;
        }
        out.name = std::string(hd.name, strnlen(hd.name, sizeof hd.name));
        out.fit = hd.fit;
    } else {
        return PinLoad::None;
    }
    sPinnedName = out.name;
    ESP_LOGI(TAG, "pinned: %s (%u bytes)", out.name.c_str(), (unsigned) out.bytes.size());
    return PinLoad::Loaded;
}

// Keep `name` across reboots: a built-in document by name, any other's bytes in flash.
// Slow (flash erase): from a task of its own.
static bool pin(const std::string& name, const std::vector<uint8_t>& bytes, bool fit, const std::string& url,
                std::string& error) {
    std::lock_guard<std::mutex> g(sPinLock);
    const EmbeddedDoc* d = findDocExact(name.c_str());
    const bool builtin = d && (size_t) (d->end - d->start) == bytes.size() &&
                         !memcmp(d->start, bytes.data(), bytes.size());
    if (!builtin) {
        const esp_partition_t* p = pinRegion();
        if (!p) { error = "no flash region to keep it in"; return false; }
        if (bytes.empty() || bytes.size() > kPinSize - 4096) { error = "too large to keep"; return false; }
        PinHeader hd = {};
        hd.magic = kPinMagic;
        hd.version = 1;
        hd.size = (uint32_t) bytes.size();
        hd.crc = esp_rom_crc32_le(0, bytes.data(), bytes.size());
        hd.fit = fit;
        strncpy(hd.name, name.c_str(), sizeof hd.name - 1);
        strncpy(hd.url, url.c_str(), sizeof hd.url - 1);
        // The header goes last: a write cut short leaves no valid document.
        const size_t span = (4096 + bytes.size() + 4095) & ~(size_t) 4095;
        if (esp_partition_erase_range(p, 0, span) != ESP_OK ||
            esp_partition_write(p, 4096, bytes.data(), bytes.size()) != ESP_OK ||
            esp_partition_write(p, 0, &hd, sizeof hd) != ESP_OK) {
            error = "flash write failed";
            return false;
        }
    }
    nvs_handle_t h;
    if (!nvsOpen(&h, true)) { error = "NVS unavailable"; return false; }
    nvs_set_str(h, "kind", builtin ? "builtin" : "flash");
    nvs_set_str(h, "name", name.c_str());
    const bool okNvs = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!okNvs) { error = "NVS write failed"; return false; }
    sPinnedName = name;
    ESP_LOGI(TAG, "pinned %s (%s, %u bytes)", name.c_str(), builtin ? "built in" : "flash", (unsigned) bytes.size());
    return true;
}

static bool unpin(bool keepValues) {
    std::lock_guard<std::mutex> g(sPinLock);
    nvs_handle_t h;
    if (!nvsOpen(&h, true)) return false;
    nvs_erase_key(h, "kind");
    nvs_erase_key(h, "name");
    if (!keepValues) {
        nvs_erase_key(h, "values");
        cJSON_Delete(sKeptValues);
        sKeptValues = nullptr;
    }
    nvs_commit(h);
    nvs_close(h);
    sPinnedName.clear();
    return true;
}

static std::string pinnedName() {
    std::lock_guard<std::mutex> g(sPinLock);
    return sPinnedName;
}

std::string pinnedNow() { return pinnedName(); }

// Variables set with persist: kept in NVS, written only when one changes (wear).
static void keepValues(cJSON* values) {
    std::lock_guard<std::mutex> g(sPinLock);
    if (!sKeptValues) sKeptValues = cJSON_CreateObject();
    bool changed = false;
    cJSON* v;
    cJSON_ArrayForEach(v, values) {
        cJSON* old = cJSON_GetObjectItem(sKeptValues, v->string);
        if (old && cJSON_Compare(old, v, true)) continue;
        cJSON_DeleteItemFromObject(sKeptValues, v->string);
        cJSON_AddItemToObject(sKeptValues, v->string, cJSON_Duplicate(v, true));
        changed = true;
    }
    if (!changed) return;
    char* json = cJSON_PrintUnformatted(sKeptValues);
    nvs_handle_t h;
    if (json && strlen(json) < 3800 && nvsOpen(&h, true)) {
        nvs_set_str(h, "values", json);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "persist: values not saved (%u bytes)", json ? (unsigned) strlen(json) : 0);
    }
    cJSON_free(json);
}

// ── Showing documents ───────────────────────────────────────────────────────────────────

// After rcstage::show: wait for the player to take it. True once it shows (the same name
// and size); false if it failed to load (the old document stays) or took too long.
static bool waitShowing(const std::string& name, size_t size, int timeoutMs) {
    for (int t = 0; t < timeoutMs; t += 50) {
        rcstage::DocInfo i = rcstage::docInfo();
        if (i.name == name && i.bytes == size) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

struct ShowJob {
    Request req;
    std::string url, name;
    std::vector<uint8_t> bytes;   // set: show these (rc.show, rc.pin); empty: download url
    bool fit = false, persist = false, pinOnly = false;
};

static void download(const std::string& url, std::vector<uint8_t>& body, int& status) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 2048;
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    status = 0;
    if (http && esp_http_client_open(http, 0) == ESP_OK) {
        int64_t len = esp_http_client_fetch_headers(http);
        status = esp_http_client_get_status_code(http);
        if (status == 200 && len <= (int64_t) kMaxDoc) {
            if (len > 0) body.reserve((size_t) len);
            uint8_t chunk[2048];
            int n;
            while ((n = esp_http_client_read(http, (char*) chunk, sizeof(chunk))) > 0 && body.size() < kMaxDoc)
                body.insert(body.end(), chunk, chunk + n);
        }
    }
    if (http) esp_http_client_cleanup(http);
}

static void runShow(ShowJob* j) {
    cJSON* result = nullptr;
    if (j->bytes.empty() && !j->url.empty()) {
        int status = 0;
        download(j->url, j->bytes, status);
        if (status != 200 || j->bytes.empty()) {
            char msg[48];
            snprintf(msg, sizeof(msg), "HTTP %d, %u bytes", status, (unsigned) j->bytes.size());
            reply(j->req, gadget_ext_error("download_failed", msg));
            return;
        }
    }
    const size_t size = j->bytes.size();
    bool showing = true;
    if (!j->pinOnly) {
        rcstage::show(j->bytes, j->name, j->fit);   // a copy: the pin below still needs them
        showing = waitShowing(j->name, size, 5000);
    }
    if (!showing) {
        result = gadget_ext_error("load_failed", "not a document this player can load; the old one stays");
    } else {
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "showing", j->name.c_str());
        cJSON_AddNumberToObject(p, "bytes", (double) size);
        cJSON_AddBoolToObject(p, "fit", j->fit);
        if (j->persist) {
            std::string error;
            const bool kept = pin(j->name, j->bytes, j->fit, j->url, error);
            cJSON_AddBoolToObject(p, "pinned", kept);
            if (!kept) cJSON_AddStringToObject(p, "pin_error", error.c_str());
        }
        result = ok(p);
    }
    reply(j->req, result);
}

static void showTask(void* arg) {
    auto* j = static_cast<ShowJob*>(arg);
    runShow(j);
    delete j;
    vTaskDelete(nullptr);
}

static cJSON* startShow(ShowJob* j) {
    // An internal-RAM stack: the pin's flash writes turn the cache (and PSRAM) off.
    if (xTaskCreate(showTask, "rc_show", 8192, j, 4, nullptr) != pdPASS) {
        delete j;
        return gadget_ext_error("out_of_memory", "could not start");
    }
    return async();
}

// A document's name from its URL: the file without ".rc", or the folder for a generic file
// name (https://.../docs/androidx-demos/maze/doc.rc -> "maze").
static std::string nameFromUrl(std::string u) {
    u = u.substr(0, u.find_first_of("?#"));
    while (!u.empty() && u.back() == '/') u.pop_back();
    size_t slash = u.find_last_of('/');
    std::string file = u.substr(slash + 1);
    if (file.size() > 3 && file.compare(file.size() - 3, 3, ".rc") == 0) file.resize(file.size() - 3);
    if ((file == "doc" || file == "index" || file.empty()) && slash != std::string::npos && slash > 0) {
        const size_t prev = u.find_last_of('/', slash - 1);
        file = u.substr(prev + 1, slash - prev - 1);
    }
    return file.empty() ? "url" : file;
}

// ── rc.screenshot ───────────────────────────────────────────────────────────────────────

// A PNG of 8-bit grey or RGB rows: the ROM's deflate with its compressor in PSRAM (its own
// tdefl_write_image_to_png_file_in_memory allocates where it can't, on the Tab5), each row
// with the Sub filter (flat colour and gradients both compress well).
static bool encodePng(const rcstage::Frame& f, std::vector<uint8_t>& png) {
    auto* c = static_cast<tdefl_compressor*>(heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM));
    if (!c) return false;
    std::vector<uint8_t> idat;
    idat.reserve((size_t) f.w * f.h * f.channels / 8);
    tdefl_init(c, [](const void* buf, int len, void* user) -> mz_bool {
        auto* v = static_cast<std::vector<uint8_t>*>(user);
        v->insert(v->end(), (const uint8_t*) buf, (const uint8_t*) buf + len);
        return MZ_TRUE;
    }, &idat, TDEFL_WRITE_ZLIB_HEADER | 64);
    const size_t stride = (size_t) f.w * f.channels;
    std::vector<uint8_t> row(stride + 1);
    bool good = true;
    for (int y = 0; y < f.h && good; y++) {
        const uint8_t* src = f.pixels.data() + y * stride;
        row[0] = 1;   // Sub
        for (size_t i = 0; i < stride; i++) row[i + 1] = (uint8_t) (src[i] - (i >= (size_t) f.channels ? src[i - f.channels] : 0));
        good = tdefl_compress_buffer(c, row.data(), row.size(), TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY;
    }
    good = good && tdefl_compress_buffer(c, nullptr, 0, TDEFL_FINISH) == TDEFL_STATUS_DONE;
    heap_caps_free(c);
    if (!good) return false;
    auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) png.push_back((uint8_t) (v >> s)); };
    auto chunk = [&](const char* type, const uint8_t* data, size_t n) {
        be32((uint32_t) n);
        const size_t start = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data, data + n);
        be32(esp_rom_crc32_le(0, png.data() + start, n + 4));
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.reserve(idat.size() + 64);
    png.insert(png.end(), sig, sig + 8);
    const uint8_t ihdr[13] = {(uint8_t) (f.w >> 24), (uint8_t) (f.w >> 16), (uint8_t) (f.w >> 8), (uint8_t) f.w,
                              (uint8_t) (f.h >> 24), (uint8_t) (f.h >> 16), (uint8_t) (f.h >> 8), (uint8_t) f.h,
                              8, (uint8_t) (f.channels == 1 ? 0 : 2), 0, 0, 0};
    chunk("IHDR", ihdr, sizeof ihdr);
    chunk("IDAT", idat.data(), idat.size());
    chunk("IEND", nullptr, 0);
    return true;
}

struct ShotJob {
    Request req;
    int w, h;
    bool gray;
};

static void runShot(ShotJob* j) {
    const int64_t t0 = esp_timer_get_time();
    auto f = std::make_unique<rcstage::Frame>();
    if (!rcstage::capture(*f, j->w, j->h, j->gray, 5000)) {
        reply(j->req, gadget_ext_error("busy", "no frame (another screenshot is running?)"));
        return;
    }
    std::vector<uint8_t> png;
    const bool encoded = encodePng(*f, png);
    f->pixels = {};
    if (!encoded) {
        reply(j->req, gadget_ext_error("out_of_memory", "PNG encoding failed"));
        return;
    }
    const size_t pngLen = png.size();
    size_t b64Len = 0;
    mbedtls_base64_encode(nullptr, 0, &b64Len, png.data(), pngLen);
    auto* b64 = (unsigned char*) heap_caps_malloc(b64Len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b64 || mbedtls_base64_encode(b64, b64Len + 1, &b64Len, png.data(), pngLen) != 0) {
        heap_caps_free(b64);
        reply(j->req, gadget_ext_error("out_of_memory", "base64 failed"));
        return;
    }
    png = {};
    b64[b64Len] = 0;
    const rcstage::DocInfo info = rcstage::docInfo();
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "format", "png-base64");
    cJSON_AddNumberToObject(p, "width", f->w);
    cJSON_AddNumberToObject(p, "height", f->h);
    cJSON_AddStringToObject(p, "color", f->channels == 1 ? "gray" : "rgb");
    cJSON_AddNumberToObject(p, "png_bytes", (double) pngLen);
    cJSON_AddNumberToObject(p, "ms", (double) ((esp_timer_get_time() - t0) / 1000));
    cJSON_AddStringToObject(p, "showing", info.name.c_str());
    cJSON_AddStringToObject(p, "data_base64", (const char*) b64);
    heap_caps_free(b64);
    reply(j->req, ok(p));
}

static void shotTask(void* arg) {
    auto* j = static_cast<ShotJob*>(arg);
    runShot(j);
    delete j;
    vTaskDeleteWithCaps(nullptr);
}

// ── device.time ─────────────────────────────────────────────────────────────────────────

static void isoTime(time_t t, bool local, char* out, size_t n) {
    struct tm tm;
    if (local) localtime_r(&t, &tm);
    else gmtime_r(&t, &tm);
    strftime(out, n, local ? "%Y-%m-%dT%H:%M:%S" : "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// Seconds east of UTC now (-25200 for PDT), from the TZ rule.
static long utcOffset(time_t t) {
    struct tm l, u;
    localtime_r(&t, &l);
    gmtime_r(&t, &u);
    long d = (l.tm_hour - u.tm_hour) * 3600L + (l.tm_min - u.tm_min) * 60L + (l.tm_sec - u.tm_sec);
    const int dayDiff = l.tm_yday - u.tm_yday;
    if (dayDiff == 1 || dayDiff < -1) d += 86400;
    else if (dayDiff == -1 || dayDiff > 1) d -= 86400;
    return d;
}

static cJSON* deviceTime() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const time_t now = tv.tv_sec;
    const rcstage::ClockState c = rcstage::clockState();
    const bool plausible = now > 1700000000;   // after Nov 2023: set by something
    cJSON* p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "epoch", now + tv.tv_usec / 1e6);
    char b[40];
    isoTime(now, false, b, sizeof b);
    cJSON_AddStringToObject(p, "utc", b);
    const long off = utcOffset(now);
    isoTime(now, true, b, sizeof b);
    char withOff[72];
    snprintf(withOff, sizeof withOff, "%s%c%02ld:%02ld", b, off < 0 ? '-' : '+', labs(off) / 3600, labs(off) % 3600 / 60);
    cJSON_AddStringToObject(p, "local", withOff);
    cJSON_AddNumberToObject(p, "utc_offset", (double) off);
    struct tm l;
    localtime_r(&now, &l);
    strftime(b, sizeof b, "%Z", &l);
    cJSON_AddStringToObject(p, "zone", b);
    cJSON_AddStringToObject(p, "tz", CONFIG_RCSTAGE_TIMEZONE);
    cJSON_AddStringToObject(p, "source", plausible ? c.source.c_str() : "none");
    cJSON_AddBoolToObject(p, "synced", c.synced);
    cJSON* ntp = cJSON_AddObjectToObject(p, "ntp");
    cJSON_AddStringToObject(ntp, "server", CONFIG_RCSTAGE_NTP_SERVER);
    if (c.lastSync) {
        cJSON_AddNumberToObject(ntp, "last_sync", (double) c.lastSync);
        cJSON_AddNumberToObject(ntp, "age_s", (double) (now - c.lastSync));
    } else {
        cJSON_AddNullToObject(ntp, "last_sync");
    }
    cJSON* rtc = cJSON_AddObjectToObject(p, "rtc");
    cJSON_AddBoolToObject(rtc, "valid", c.rtc);   // false: no RTC, or it lost power
    if (c.rtc) {
        cJSON_AddNumberToObject(rtc, "epoch", (double) c.rtcEpoch);
        cJSON_AddNumberToObject(rtc, "drift_s", (double) (c.rtcEpoch - now));
    }
    cJSON_AddNumberToObject(p, "uptime_s", (double) (esp_timer_get_time() / 1000000));
    return ok(p);
}

// ── device.motion ───────────────────────────────────────────────────────────────────────

static cJSON* deviceMotion() {
    const board::Motion m = board::readMotion();
    if (!m.valid) return gadget_ext_error("unsupported", "this board has no motion sensor (or it did not start)");
    cJSON* p = cJSON_CreateObject();
    cJSON* a = cJSON_AddObjectToObject(p, "accel");
    cJSON_AddNumberToObject(a, "x", std::round(m.ax * 100.0) / 100.0);
    cJSON_AddNumberToObject(a, "y", std::round(m.ay * 100.0) / 100.0);
    cJSON_AddNumberToObject(a, "z", std::round(m.az * 100.0) / 100.0);
    cJSON* g = cJSON_AddObjectToObject(p, "gyro");
    cJSON_AddNumberToObject(g, "x", std::round(m.gx * 1000.0) / 1000.0);
    cJSON_AddNumberToObject(g, "y", std::round(m.gy * 1000.0) / 1000.0);
    cJSON_AddNumberToObject(g, "z", std::round(m.gz * 1000.0) / 1000.0);
    // Which way gravity pulls: the axis it dominates, as the screen sees it.
    const float ax = std::fabs(m.ax), ay = std::fabs(m.ay), az = std::fabs(m.az);
    const char* o = az > ax && az > ay ? (m.az > 0 ? "face_up" : "face_down")
                  : ay > ax ? (m.ay > 0 ? "upright" : "upside_down")
                  : (m.ax > 0 ? "left_edge_down" : "right_edge_down");
    cJSON_AddStringToObject(p, "orientation", o);
    const float mag = std::sqrt(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
    cJSON_AddNumberToObject(p, "g", std::round(mag / 9.80665 * 100.0) / 100.0);
    return ok(p);
}

// ── device.beep ─────────────────────────────────────────────────────────────────────────

struct Beeps { int hz, ms, count, gapMs; };

static void beepTask(void* arg) {
    auto* b = static_cast<Beeps*>(arg);
    for (int i = 0; i < b->count; i++) {
        board::beep(b->hz, b->ms);
        vTaskDelay(pdMS_TO_TICKS(b->ms + b->gapMs));
    }
    delete b;
    vTaskDelete(nullptr);
}

static cJSON* deviceBeep(cJSON* params) {
    if (!*board::kSound) return gadget_ext_error("unsupported", "this board has no speaker or buzzer");
    Beeps b{(int) number(params, "hz", 880), (int) number(params, "ms", 200), (int) number(params, "count", 1),
            (int) number(params, "gap_ms", 120)};
    b.hz = std::clamp(b.hz, 50, 10000);
    b.ms = std::clamp(b.ms, 10, 5000);
    b.count = std::clamp(b.count, 1, 10);
    b.gapMs = std::clamp(b.gapMs, 20, 2000);
    bool started;
    if (b.count == 1) started = board::beep(b.hz, b.ms);
    else started = xTaskCreate(beepTask, "beeps", 2048, new Beeps(b), 3, nullptr) == pdPASS;
    if (!started) return gadget_ext_error("unavailable", "the sound output did not start");
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "on", board::kSound);
    cJSON_AddNumberToObject(p, "hz", b.hz);
    cJSON_AddNumberToObject(p, "ms", b.ms);
    cJSON_AddNumberToObject(p, "count", b.count);
    return ok(p);
}

// ── rc.info ─────────────────────────────────────────────────────────────────────────────

static cJSON* addSize(cJSON* parent, const char* key, double w, double h) {
    cJSON* o = cJSON_AddObjectToObject(parent, key);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    return o;
}

static cJSON* rcInfo() {
    const rcstage::DocInfo d = rcstage::docInfo();
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "board", board::kName);
    cJSON_AddStringToObject(p, "board_name", board::kDisplayName);
    if (const std::string ssid = wifiSsid(); !ssid.empty()) cJSON_AddStringToObject(p, "wifi_ssid", ssid.c_str());
    else cJSON_AddNullToObject(p, "wifi_ssid");
    addSize(p, "panel", board::kWidth, board::kHeight);
    cJSON_AddNumberToObject(p, "density", board::kDensity);
    addSize(p, "logical", std::floor(board::kWidth / board::kDensity), std::floor(board::kHeight / board::kDensity));
    addSize(p, "render", d.renderW, d.renderH);
    cJSON_AddBoolToObject(p, "round", board::kRound);
    cJSON_AddBoolToObject(p, "eink", board::kEink);
    if (board::kGrays) cJSON_AddNumberToObject(p, "grays", board::kGrays);
    else cJSON_AddNullToObject(p, "grays");
    cJSON_AddStringToObject(p, "sound", board::kSound);
    cJSON* doc = cJSON_AddObjectToObject(p, "document");
    cJSON_AddStringToObject(doc, "name", d.name.c_str());
    addSize(doc, "size", d.dpW, d.dpH);
    cJSON_AddBoolToObject(doc, "fit", d.fit);
    cJSON_AddNumberToObject(doc, "scale", std::round(d.scale * 1000.0) / 1000.0);
    cJSON_AddNumberToObject(doc, "bytes", (double) d.bytes);
    const std::string pinned = pinnedName();
    if (pinned.empty()) cJSON_AddNullToObject(p, "pinned");
    else cJSON_AddStringToObject(p, "pinned", pinned.c_str());
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(p, "firmware", app ? app->version : "?");
    sleepInfo(p);
    return ok(p);
}

// ── rc.get / rc.set ─────────────────────────────────────────────────────────────────────

static cJSON* rcGet(cJSON* params) {
    const char* name = str(params, "name");
    std::vector<rcstage::NamedValue> vals;
    if (!rcstage::namedValues(vals, 2000)) return gadget_ext_error("busy", "the player did not answer; try again");
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "showing", rcstage::currentName().c_str());
    auto add = [](cJSON* o, const rcstage::NamedValue& v) {
        cJSON_AddStringToObject(o, "name", v.name.c_str());
        cJSON_AddStringToObject(o, "type", v.type.c_str());
        if (v.type == "number" || v.type == "integer") cJSON_AddNumberToObject(o, "value", strtod(v.value.c_str(), nullptr));
        else cJSON_AddStringToObject(o, "value", v.value.c_str());
    };
    if (name && *name) {
        for (auto& v : vals)
            if (v.name == name) { add(p, v); return ok(p); }
        cJSON_Delete(p);
        return gadget_ext_error("not_declared", "the showing document has no variable by that name; rc.get {} lists them");
    }
    cJSON* list = cJSON_AddArrayToObject(p, "variables");
    for (auto& v : vals) {
        cJSON* o = cJSON_CreateObject();
        add(o, v);
        cJSON_AddItemToArray(list, o);
    }
    return ok(p);
}

static cJSON* rcSet(cJSON* params) {
    // {name, value} or {values: {name: value, ...}} (an object, or that object as a string).
    cJSON* values = cJSON_CreateObject();
    cJSON* in = cJSON_GetObjectItem(params, "values");
    cJSON* parsed = cJSON_IsString(in) ? cJSON_Parse(in->valuestring) : nullptr;
    if (parsed) in = parsed;
    if (cJSON_IsObject(in)) {
        cJSON* v;
        cJSON_ArrayForEach(v, in) cJSON_AddItemToObject(values, v->string, cJSON_Duplicate(v, true));
    }
    cJSON_Delete(parsed);
    const char* name = str(params, "name");
    if (name && *name && cJSON_GetObjectItem(params, "value"))
        cJSON_AddItemToObject(values, name, cJSON_Duplicate(cJSON_GetObjectItem(params, "value"), true));
    if (!cJSON_GetArraySize(values)) {
        cJSON_Delete(values);
        return gadget_ext_error("missing_param", "name and value, or values, are required");
    }
    const std::vector<std::string> declared = rcstage::namedVariableNames();
    cJSON* p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "showing", rcstage::currentName().c_str());
    cJSON* applied = cJSON_AddArrayToObject(p, "applied");
    cJSON* missing = cJSON_AddArrayToObject(p, "not_declared");
    cJSON* bad = nullptr;
    cJSON* v;
    cJSON_ArrayForEach(v, values) {
        std::string text;
        if (!valueText(v, text)) {
            if (!bad) bad = cJSON_AddArrayToObject(p, "bad_value");
            cJSON_AddItemToArray(bad, cJSON_CreateString(v->string));
            continue;
        }
        rcstage::setNamed(v->string, text);
        const bool has = std::find(declared.begin(), declared.end(), v->string) != declared.end();
        cJSON_AddItemToArray(has ? applied : missing, cJSON_CreateString(v->string));
    }
    const bool persist = flag(params, "persist");
    if (persist) keepValues(values);
    cJSON_AddBoolToObject(p, "persisted", persist);
    cJSON_Delete(values);
    return ok(p);
}

// ── rc.last_touch / rc.events ───────────────────────────────────────────────────────────

static cJSON* lastTouch() {
    const rcstage::TouchInfo t = rcstage::lastTouch();
    cJSON* p = cJSON_CreateObject();
    cJSON_AddBoolToObject(p, "down", t.down);
    cJSON_AddNumberToObject(p, "presses", t.downs);
    if (!t.atUs) {
        cJSON_AddNullToObject(p, "age_ms");
        return ok(p);
    }
    cJSON_AddNumberToObject(p, "x", std::round(t.x * 10.0) / 10.0);
    cJSON_AddNumberToObject(p, "y", std::round(t.y * 10.0) / 10.0);
    cJSON_AddNumberToObject(p, "panel_x", t.panelX);
    cJSON_AddNumberToObject(p, "panel_y", t.panelY);
    cJSON_AddNumberToObject(p, "age_ms", std::round(ageMs(t.atUs)));
    return ok(p);
}

static cJSON* rcEvents(cJSON* params) {
    uint32_t latest = 0;
    const auto evs = rcstage::events((uint32_t) number(params, "since", 0), &latest);
    cJSON* p = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(p, "events");
    for (auto& e : evs) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", e.seq);
        cJSON_AddStringToObject(o, "type", e.type.c_str());
        cJSON_AddNumberToObject(o, "age_ms", std::round(ageMs(e.atUs)));
        if (e.type == "tap" || e.type == "long_press") {
            cJSON_AddNumberToObject(o, "x", std::round(e.x * 10.0) / 10.0);
            cJSON_AddNumberToObject(o, "y", std::round(e.y * 10.0) / 10.0);
        }
        if (!e.name.empty()) cJSON_AddStringToObject(o, "name", e.name.c_str());
        if (!e.value.empty()) cJSON_AddStringToObject(o, "value", e.value.c_str());
        cJSON_AddItemToArray(list, o);
    }
    cJSON_AddNumberToObject(p, "latest", latest);
    return ok(p);
}

// ── Registration ────────────────────────────────────────────────────────────────────────
// The whole link.register must fit in 8 KB: descriptions stay short; docs/MUSE_COMMANDS.md
// has the rest.

void registerCommands(cJSON* c) {
    auto add = gadget_ext_add_command;
    add(c, "rc.list", "List built-in documents; which is showing and pinned.", nullptr, nullptr);
    add(c, "rc.show", "Show a built-in document (rc.list).",
        params({{"name", "string", "Document"}}), params({{"persist", "boolean", "Pin: show it after reboot"}}));
    add(c, "rc.show_url", "Download a .rc (<=4 MB) and show it; replies once it shows.",
        params({{"url", "string", "http(s) URL"}}),
        params({{"fit", "boolean", "Lay out at its own size, scaled to fit"},
                {"persist", "boolean", "Pin: show it after reboot"}, {"name", "string", "Name it"}}));
    add(c, "rc.set", "Set named variables of the showing document; says which it declares.",
        nullptr,
        params({{"name", "string", "Variable"}, {"value", "string", "Number, #RRGGBB or text"},
                {"values", "string", "JSON object {name: value} for several"},
                {"persist", "boolean", "Restore them after reboot"}}));
    add(c, "rc.get", "Read named variables as drawn; no name lists them all.", nullptr,
        params({{"name", "string", "Variable"}}));
    add(c, "rc.pin", "Show the showing document after reboot too.", nullptr, nullptr);
    add(c, "rc.unpin", "Boot to the start screen again; forget persisted values.", nullptr,
        params({{"keep_values", "boolean", "Keep persisted values"}}));
    add(c, "rc.screenshot", "PNG of the screen as composed (base64).", nullptr,
        params({{"width", "integer", "Pixels (default: logical width)"},
                {"gray", "boolean", "Greys (default on e-ink)"}}));
    add(c, "rc.info", "Screen geometry and the showing document's size.", nullptr, nullptr);
    add(c, "rc.last_touch", "Last touch: x, y (document dp), age_ms.", nullptr, nullptr);
    add(c, "rc.events", "Taps, long presses, buttons, document actions since seq.", nullptr,
        params({{"since", "integer", "Last seq seen (0: all kept)"}}));
    add(c, "rc.banner", "Banner over the screen; an empty title hides it.",
        params({{"title", "string", "Headline"}}), params({{"detail", "string", "Second line"}}));
    add(c, "device.time", "System time, its source (ntp/rtc/none), RTC and NTP state.", nullptr, nullptr);
    if (*board::kSound)
        add(c, "device.beep", "Play a tone.", nullptr,
            params({{"hz", "integer", "Pitch (880)"}, {"ms", "integer", "Length (200)"},
                    {"count", "integer", "Repeats (1)"}, {"gap_ms", "integer", "Between (120)"}}));
    add(c, "device.wifi", "Wi-Fi: the network joined, the saved ones; add, forget or join one.", nullptr,
        params({{"add", "string", "SSID to save, to join when in range"},
                {"password", "string", "With add: its password (none: open)"},
                {"hidden", "boolean", "With add: it doesn't broadcast its name"},
                {"forget", "string", "SSID to forget"},
                {"join", "string", "Saved SSID to join now (the session drops ~15 s)"}}));
    add(c, "device.motion", "Accelerometer (m/s2) and gyro (rad/s) in screen axes.", nullptr, nullptr);
    if (board::kEink)   // the boards whose sleep is written: the PaperS3
        add(c, "device.sleep", "Sleep N s, or auto: between the document's frames, checking in every check_in_s.",
            nullptr,
            params({{"seconds", "integer", "How long (default: to the document's next frame)"},
                    {"mode", "string", "off (default), deep, light (keeps RAM)"},
                    {"auto", "boolean", "Keep sleeping between frames; false stops"},
                    {"check_in_s", "integer", "Auto: connect this often (900)"},
                    {"awake_s", "integer", "Auto: awake after connecting (60, min 60)"},
                    {"hold", "boolean", "Auto: at check-ins stay awake until resume"},
                    {"hold_max_s", "integer", "Hold at most this long (3600; 0 no limit)"},
                    {"resume", "boolean", "End the check-in; back to sleeping"}}));
    if (board::kEink)
        add(c, "device.battery_log", "Battery reading at each auto-sleep check-in, and the drain.", nullptr,
            params({{"clear", "boolean", "Start a new log"}}));
    if (board::kEink)
        add(c, "display.refresh", "Full e-ink refresh: clears ghosting.", nullptr,
            params({{"mode", "string", "full"}}));
}

// ── device.health extras ────────────────────────────────────────────────────────────────

void health(cJSON* h) {
    const rcstage::DeviceState s = rcstage::deviceState();
    const board::Power& pw = s.power;
    cJSON* m = cJSON_GetObjectItem(h, "metrics");
    if (m) {
        // Link has no battery gauge of its own here: fill in the board's.
        auto fill = [&](const char* key, cJSON* v) {
            cJSON* old = cJSON_GetObjectItem(m, key);
            if (old && !cJSON_IsNull(old)) { cJSON_Delete(v); return; }
            if (old) cJSON_ReplaceItemInObject(m, key, v);
            else cJSON_AddItemToObject(m, key, v);
        };
        if (pw.valid) {
            fill("battery_pct", pw.batteryLevel >= 0 ? cJSON_CreateNumber(pw.batteryLevel) : cJSON_CreateNull());
            fill("battery_mv", pw.batteryVolts > 0 ? cJSON_CreateNumber(std::round(pw.batteryVolts * 1000))
                                                   : cJSON_CreateNull());
            fill("charging", cJSON_CreateBool(pw.charging));
            fill("usb_power", pw.powerState >= 1 ? cJSON_CreateBool(true)
                              : pw.powerState == 0 ? cJSON_CreateBool(false) : cJSON_CreateNull());
            if (pw.batteryMilliamps != 0) cJSON_AddNumberToObject(m, "battery_ma", std::round(pw.batteryMilliamps));
        }
        if (s.wifiRssi) cJSON_AddNumberToObject(m, "wifi_rssi", s.wifiRssi);
        cJSON_AddNumberToObject(m, "temperature_c", std::round(s.temperatureC * 10.0) / 10.0);
    }
    if (esp_netif_t* n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(n, &info) == ESP_OK && info.ip.addr) {
            char b[16];
            snprintf(b, sizeof b, IPSTR, IP2STR(&info.ip));
            cJSON_AddStringToObject(h, "ip", b);
        }
    }
    const std::string ssid = wifiSsid();
    if (ssid.empty()) cJSON_AddNullToObject(h, "wifi_ssid");
    else cJSON_AddStringToObject(h, "wifi_ssid", ssid.c_str());
    cJSON_AddStringToObject(h, "board", board::kName);
    cJSON_AddStringToObject(h, "woke", board::wakeReason());
    cJSON_AddStringToObject(h, "showing", rcstage::currentName().c_str());
    const std::string pinned = pinnedName();
    if (pinned.empty()) cJSON_AddNullToObject(h, "pinned");
    else cJSON_AddStringToObject(h, "pinned", pinned.c_str());
    sleepInfo(h);
}

// ── Dispatch ────────────────────────────────────────────────────────────────────────────

cJSON* command(const char* command, cJSON* params, const char* requestId, noise_ctrl_session_generation_t gen) {
    const bool rc = !strncmp(command, "rc.", 3);
    if (!rc && strcmp(command, "device.time") && strcmp(command, "device.beep") &&
        strcmp(command, "device.motion") && strcmp(command, "display.refresh") && strcmp(command, "device.sleep") &&
        strcmp(command, "device.battery_log") && strcmp(command, "device.wifi"))
        return nullptr;
    noteActivity();
    if (rc && !stageUp()) return gadget_ext_error("unavailable", "the display did not start");
    const Request req{requestId ? requestId : "", gen};

    if (!strcmp(command, "rc.list")) {
        cJSON* p = cJSON_CreateObject();
        cJSON* docs = cJSON_AddArrayToObject(p, "documents");
        for (auto& d : kEmbeddedDocs) cJSON_AddItemToArray(docs, cJSON_CreateString(d.name));
        cJSON_AddStringToObject(p, "showing", rcstage::currentName().c_str());
        const std::string pinned = pinnedName();
        if (pinned.empty()) cJSON_AddNullToObject(p, "pinned");
        else cJSON_AddStringToObject(p, "pinned", pinned.c_str());
        return ok(p);
    }
    if (!strcmp(command, "rc.show")) {
        const char* name = str(params, "name");
        const EmbeddedDoc* d = name ? findDoc(name) : nullptr;
        if (!d) return gadget_ext_error("not_found", "no such document; see rc.list");
        ESP_LOGI(TAG, "rc.show %s", d->name);
        auto* j = new ShowJob{req, {}, d->name, std::vector<uint8_t>(d->start, d->end), d->fit, flag(params, "persist")};
        return startShow(j);
    }
    if (!strcmp(command, "rc.show_url")) {
        const char* url = str(params, "url");
        if (!url || !*url) return gadget_ext_error("missing_param", "url is required");
        ESP_LOGI(TAG, "rc.show_url %s", url);
        const char* given = str(params, "name");
        const std::string u(url), name = given && *given ? given : nameFromUrl(u);
        auto* j = new ShowJob{req, u, name, {}, flag(params, "fit"), flag(params, "persist")};
        return startShow(j);
    }
    if (!strcmp(command, "rc.pin")) {
        const rcstage::DocInfo d = rcstage::docInfo();
        auto bytes = rcstage::docBytes();
        if (!bytes) return gadget_ext_error("unavailable", "nothing showing");
        auto* j = new ShowJob{req, {}, d.name, *bytes, d.fit, true, true};
        return startShow(j);
    }
    if (!strcmp(command, "rc.unpin")) {
        if (!unpin(flag(params, "keep_values"))) return gadget_ext_error("unavailable", "NVS unavailable");
        return ok();
    }
    if (!strcmp(command, "rc.set")) return rcSet(params);
    if (!strcmp(command, "rc.get")) return rcGet(params);
    if (!strcmp(command, "rc.banner")) {
        const char *title = str(params, "title"), *detail = str(params, "detail");
        rcstage::setBanner(title ? title : "", detail ? detail : "", 0xFF42A5F5);
        return ok();
    }
    if (!strcmp(command, "rc.screenshot")) {
        const rcstage::DocInfo d = rcstage::docInfo();
        const int logicalW = (int) (board::kWidth / board::kDensity);
        const int w = std::clamp((int) number(params, "width", logicalW), 16, std::max(16, d.renderW));
        const int h = std::max(1, (int) std::lround((double) w * d.renderH / std::max(1, d.renderW)));
        auto* j = new ShotJob{req, w, h, flag(params, "gray", board::kEink)};
        // Deflate wants a deep stack (16 KB); PSRAM's fine, as nothing here writes flash.
        if (xTaskCreateWithCaps(shotTask, "rc_shot", 16384, j, 3, nullptr, MALLOC_CAP_SPIRAM) != pdPASS) {
            delete j;
            return gadget_ext_error("out_of_memory", "could not start");
        }
        return async();
    }
    if (!strcmp(command, "rc.info")) return rcInfo();
    if (!strcmp(command, "rc.last_touch")) return lastTouch();
    if (!strcmp(command, "rc.events")) return rcEvents(params);
    if (!strcmp(command, "device.time")) return deviceTime();
    if (!strcmp(command, "device.motion")) return deviceMotion();
    if (!strcmp(command, "device.beep")) return deviceBeep(params);
    if (!strcmp(command, "device.sleep")) return sleepCommand(params);
    if (!strcmp(command, "device.wifi")) return wifiCommand(params);
    if (!strcmp(command, "device.battery_log")) return batteryLogCommand(params);
    if (!strcmp(command, "display.refresh")) {
        const char* mode = str(params, "mode");
        if (mode && *mode && strcmp(mode, "full")) return gadget_ext_error("bad_param", "mode: full");
        if (!board::displayRefresh()) return gadget_ext_error("unsupported", "only e-ink panels need a refresh");
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "mode", "full");
        return ok(p);
    }
    return gadget_ext_error("unknown_command", command);
}

// "api <command> [json]" on the serial console: run a command here, print what it returns
// (async ones print when they finish). On a task of its own, as link.invoke runs on the
// Noise session's: some commands wait for the player task, which reads the console.
static void consoleTask(void* arg) {
    auto* rest = static_cast<std::string*>(arg);
    if (*rest == "register") {
        cJSON* c = cJSON_CreateObject();
        registerCommands(c);
        char* json = cJSON_PrintUnformatted(c);
        printf("API REGISTER %u bytes for %d commands (all of link.register must fit in 8192)\n",
               json ? (unsigned) strlen(json) : 0, cJSON_GetArraySize(c));
        cJSON_free(json);
        cJSON_Delete(c);
        delete rest;
        vTaskDelete(nullptr);
        return;
    }
    if (*rest == "device.health") {
        // Link answers this one itself; show what this project adds to it.
        cJSON* h = cJSON_CreateObject();
        cJSON* m = cJSON_AddObjectToObject(h, "metrics");
        for (const char* k : {"battery_pct", "battery_mv", "charging", "usb_power"}) cJSON_AddNullToObject(m, k);
        health(h);
        cJSON* r = ok(h);
        printResult(r);
        cJSON_Delete(r);
        delete rest;
        vTaskDelete(nullptr);
        return;
    }
    const size_t sp = rest->find(' ');
    const std::string name = rest->substr(0, sp);
    cJSON* params = sp == std::string::npos ? cJSON_CreateObject() : cJSON_Parse(rest->c_str() + sp + 1);
    delete rest;
    if (!params) {
        printf("API RESULT {\"ok\":false,\"error\":{\"code\":\"bad_json\"}}\n");
        vTaskDelete(nullptr);
        return;
    }
    cJSON* r = command(name.c_str(), params, kConsoleRequest, 0);
    if (!r) r = gadget_ext_error("unknown_command", name.c_str());
    if (cJSON_IsTrue(cJSON_GetObjectItem(r, "_async"))) cJSON_Delete(r);
    else {
        printResult(r);
        cJSON_Delete(r);
    }
    cJSON_Delete(params);
    vTaskDelete(nullptr);
}

bool consoleApi(const std::string& line) {
    if (line.compare(0, 4, "api ") != 0) return false;
    auto* rest = new std::string(line.substr(4));
    if (xTaskCreate(consoleTask, "rc_api", 6144, rest, 3, nullptr) != pdPASS) {
        delete rest;
        printf("API RESULT {\"ok\":false,\"error\":{\"code\":\"out_of_memory\"}}\n");
    }
    return true;
}

}  // namespace muserc

// ── The SDK's hooks ─────────────────────────────────────────────────────────────────────

extern "C" void gadget_ext_register_commands(cJSON* commands) { muserc::registerCommands(commands); }

extern "C" cJSON* gadget_ext_command(const char* command, cJSON* params, const char* requestId,
                                     noise_ctrl_session_generation_t generation) {
    return muserc::command(command, params, requestId, generation);
}

extern "C" void gadget_ext_health(cJSON* health) { muserc::health(health); }
