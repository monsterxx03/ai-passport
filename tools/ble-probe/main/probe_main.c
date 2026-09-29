// BLE 配对探针（临时工具，不是产品固件）
//
// 它要回答四个问题，答案决定 BLE 链路怎么做（设计文档 §附录）：
//   1. macOS 对 DISPLAY_ONLY + MITM 的 peripheral，会不会弹「输入 6 位码」的框？
//   2. 配对成功后 encrypted / authenticated / bonded 三个位是否都为 1？
//   3. 断开重连是否免配对（bond 真的生效）？重启之后呢？
//   4. 设备侧把 bond 删掉之后，旧电脑会不会被正确拒绝、能不能重新配对？
//
// 为什么这么小：第 1 条不依赖我们的界面，只依赖「peripheral 的 IO 能力和安全要求」
// 加一个会触发配对的 central 动作。所以 passkey 固定成 123456 打在串口日志里，
// 探针不接屏幕、不做协议——UI 的工作留到事实确定之后。
//
// 为什么特征是 READ_AUTHEN：它要求一条**经过认证**的加密链路，所以 central 一读就会
// 触发配对，而且 Just Works 那种不认证的配对**满足不了它**——这正是我们要验证的边界。
// 产品固件里将来也是同一个约束（收到任何协议行之前先看安全状态）。

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "probe";

// 探针的固定 passkey：不接屏幕，照着日志输。产品固件里每个配对随机生成一个
// （那才是安全的做法——常显的固定码等于贴了张纸条）。
#define PROBE_PASSKEY 123456U
#define PROBE_NAME "Tachi-Badge-PROBE"
#define PROBE_SVC_UUID 0xABCD
#define PROBE_CHR_UUID 0xABCE

static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// 期望被 central 读到的东西；读它需要一条已认证的加密链路。
static const char s_value[] = "tachi-badge-probe-v1";

// log_sec_state 把安全状态打成一行。三个位就是设计要的全部依据：
//   encrypted=0          → 链路是明文，一律不许说协议
//   authenticated=0      → 只做了 Just Works，攻击者能中间人
//   bonded=0             → 没有留 bond，每次重连都要重新配对
static void log_sec_state(const char *when)
{
    struct ble_gap_conn_desc desc;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
        ESP_LOGW(TAG, "%s: 连接已不在", when);
        return;
    }
    ESP_LOGI(TAG, "PROBE sec[%s] encrypted=%d authenticated=%d bonded=%d key_size=%d",
             when, desc.sec_state.encrypted, desc.sec_state.authenticated,
             desc.sec_state.bonded, desc.sec_state.key_size);
}

static void log_bond_count(const char *when)
{
    int count = 0;

    if (ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &count) == 0) {
        ESP_LOGI(TAG, "PROBE bonds[%s] = %d", when, count);
    }
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        log_sec_state("read");
        if (os_mbuf_append(ctxt->om, s_value, strlen(s_value)) == 0) {
            ESP_LOGI(TAG, "PROBE 读成功（说明链路已认证加密）");
            return 0;
        }
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[64];
        uint16_t len = 0;

        log_sec_state("write");
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1U, &len) == 0) {
            buf[len] = '\0';
            ESP_LOGI(TAG, "PROBE 收到写入: %s", buf);
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(PROBE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(PROBE_CHR_UUID),
                // 读要「认证过的加密」，写要「加密」：前者是设计里的门槛，
                // 后者用来验证加密链路上真的能传数据。
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg);

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params params = { 0 };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)PROBE_NAME;
    fields.name_len = strlen(PROBE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        return rc;
    }
    // 和 demo_ble.c 的唯一区别：这里要**可连接**（它那个是纯广播）。
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "PROBE 广播中: %s", PROBE_NAME);
    }
    return rc;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGW(TAG, "PROBE 连上了 handle=%d", s_conn_handle);
            log_sec_state("连接时");
        } else {
            ESP_LOGW(TAG, "PROBE 连接失败 status=%d，继续广播", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "PROBE 断开 reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        log_bond_count("断开后");
        advertise(); // 断开就继续广播，方便反复重连
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGW(TAG, "PROBE 加密状态变化 status=%d", event->enc_change.status);
        log_sec_state("加密后");
        log_bond_count("加密后");
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // 配对走到「要一个 6 位码」这一步。DISPLAY_ONLY 的角色里，这个码由我们自己
        // 生成、显示、并交回协议栈（真实设计里每次配对随机一个）。
        ESP_LOGW(TAG, "PROBE *** PASSKEY = %06u ***（在电脑上输入它）", PROBE_PASSKEY);
        {
            struct ble_sm_io io = {
                .action = BLE_SM_IOACT_DISP,
                .passkey = PROBE_PASSKEY,
            };
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 对方重新要求配对（它那边把 bond 删了）。把本机那条陈旧 bond 丢掉再重配，
        // 否则就是那个经典死锁：双方都以为对方还认旧密钥。
        ESP_LOGW(TAG, "PROBE 对方要求重新配对，删掉本机的陈旧 bond");
        {
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "PROBE 订阅变化 notify=%d", event->subscribe.cur_notify);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "PROBE MTU = %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "PROBE 拿不到本机地址");
        return;
    }
    log_bond_count("同步后");
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "PROBE 协议栈复位 reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();       // 直到 nimble_port_stop
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGW(TAG, "PROBE 启动：%s", PROBE_NAME);
    ESP_ERROR_CHECK(nimble_port_init());

    // IO 能力 = 只能显示（有屏没键盘）→ 配对走 Passkey Entry；
    // MITM + SC 是「不许 Just Works」这条要求的全部写法。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set(PROBE_NAME) != 0) {
        ESP_LOGE(TAG, "PROBE 设备名设置失败");
        return;
    }

    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "PROBE GATT 注册失败 rc=%d", rc);
        return;
    }

    nimble_port_freertos_init(host_task);
}
