#include "ble.h"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_ESP_HOSTED_HOST_FEAT_BT
#include "esp_hosted_bt_host_stack.h"
#endif
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char* TAG = "ble";

namespace ble {

static bool sStarted = false;
static int sSeen = 0;
static uint8_t sAddrType;

static void advertise();

static int onGap(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_DISC: {
            sSeen++;
            ble_hs_adv_fields f;
            if (ble_hs_adv_parse_fields(&f, ev->disc.data, ev->disc.length_data) == 0 && f.name_len) {
                printf("  ble: %02x:%02x:%02x:%02x:%02x:%02x %4d dBm  %.*s\n", ev->disc.addr.val[5],
                       ev->disc.addr.val[4], ev->disc.addr.val[3], ev->disc.addr.val[2], ev->disc.addr.val[1],
                       ev->disc.addr.val[0], ev->disc.rssi, f.name_len, (const char*) f.name);
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            printf("ble: scan done, %d advertisements\n", sSeen);
            advertise();
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "connect status=%d", ev->connect.status);
            if (ev->connect.status != 0) advertise();
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnect reason=%d", ev->disconnect.reason);
            advertise();
            return 0;
        default:
            return 0;
    }
}

static void advertise() {
    ble_hs_adv_fields f = {};
    const char* name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t*) name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(sAddrType, nullptr, BLE_HS_FOREVER, &p, onGap, nullptr);
    printf("ble: advertising as '%s' rc=%d\n", name, rc);
}

static void onSync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &sAddrType);
    uint8_t a[6] = {};
    ble_hs_id_copy_addr(sAddrType, a, nullptr);
    printf("ble: controller up, addr %02x:%02x:%02x:%02x:%02x:%02x; scanning 5 s\n", a[5], a[4], a[3], a[2], a[1],
           a[0]);
    ble_gap_disc_params dp = {};
    dp.passive = 1;
    dp.filter_duplicates = 1;
    sSeen = 0;
    ble_gap_disc(sAddrType, 5000, &dp, onGap, nullptr);
}

static void hostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void start() {
    if (sStarted) { printf("ble: already started\n"); return; }
    // ESP_HOSTED_BT_HOST_STACK_CONFIG_DEFAULT() is a C compound literal; spell it out for C++.
#if CONFIG_ESP_HOSTED_HOST_FEAT_BT
    esp_hosted_bt_host_stack_cfg_t cfg = {};
    cfg.stack = ESP_HOSTED_BT_HOST_STACK_NIMBLE;
    // The Tab5's factory C6 firmware (esp-hosted, reports 0.0.0) predates the controller
    // init/enable RPCs (no reply to msg 387) but brings its BT controller up at boot, so only
    // bind NimBLE to the HCI pipe.
    cfg.bring_up_controller = false;
    cfg.controller_ready_timeout_ms = EH_BT_CTRL_DEFAULT_READY_TIMEOUT_MS;
    esp_err_t err = esp_hosted_bt_host_stack_setup(&cfg);
    if (err != ESP_OK) { printf("ble: hosted setup failed: %s\n", esp_err_to_name(err)); return; }
#else
    esp_err_t err;
#endif
    err = nimble_port_init();
    if (err != ESP_OK) { printf("ble: nimble_port_init failed: %s\n", esp_err_to_name(err)); return; }
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = [](int reason) { ESP_LOGW(TAG, "host reset, reason %d", reason); };
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("Tab5-RC");
    nimble_port_freertos_init(hostTask);
    sStarted = true;
}

}  // namespace ble
