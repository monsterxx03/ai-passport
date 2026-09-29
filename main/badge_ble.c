// badge_ble —— 设备侧的 BLE 链路（GATT peripheral）。
//
// 与 badge_link.c 的关系：两者是**同一件事的两个传输**，对上层完全一样——一个
// 「收到一行」的回调，加一个「发一行」的函数。状态机（badge_state）不知道自己在跟谁说话。
//
// 三件只有 BLE 才需要、而且已经实测过的事（见 tachi 仓库设计文档附录 A.2）：
//
//   1. **安全门禁**：链路建立时是**明文**的，几百毫秒后才升级（实测 420ms）。所以「连上」
//      不等于「安全」——在任何一行协议被接受之前，必须 encrypted && authenticated 都为真。
//      这是硬门槛，不是警告：这条链路上的一个回答就是在批准一条要执行的命令。
//   2. **配对**：DISPLAY_ONLY + MITM + bonding + SC。设备有屏没键盘，所以走 Passkey Entry：
//      每次配对生成一个 6 位码显示在屏上，用户在电脑上输入。macOS 会主动弹那个框（实测）。
//   3. **忘记那台电脑**：电脑侧留着旧 bond、而这里忘了它时，重连会「连上即断」且电脑
//      **不会**自动重新配对（实测）。所以必须有一个明确的入口把 bond 删掉、重新广播。
//
// 分包/组包在这里：GATT 一次只能写 MTU-3 字节（实测 MTU=256），而一条 ask 可以到 2KB 出头，
// 所以入方向累积到换行为止，出方向按 MTU 切片。协议那一层（一行 JSON）完全不知道这件事。

#include "badge_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static const char *TAG = "badge_ble";

// 服务与特征：RX = 主机写进来，TX = 我们 notify 出去。
// 两个都要求**认证过的加密**（AUTHEN），这样「安全门禁」不只是我们自己代码里的判断，
// 协议栈也会在属性层面拒绝明文访问 —— 两道。
#define BADGE_BLE_SVC_UUID 0xFFF0
#define BADGE_BLE_RX_UUID 0xFFF1
#define BADGE_BLE_TX_UUID 0xFFF2

#define BADGE_BLE_NAME "Tachi-Badge"
#define BADGE_BLE_LINE_MAX 4096U  // 与 badge_link 的 BADGE_LINK_LINE_MAX 保持一致
#define BADGE_BLE_LINE_PREFIX "@@"
#define BADGE_BLE_LINE_PREFIX_LENGTH 2U
// 没协商出 MTU 之前的保守值：BLE 默认 ATT_MTU=23，减去 3 字节头。
#define BADGE_BLE_DEFAULT_CHUNK 20U

static badge_ble_line_cb_t s_callback;
static void *s_context;

static uint8_t s_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint16_t s_chunk = BADGE_BLE_DEFAULT_CHUNK;
static bool s_subscribed;
static volatile bool s_secured;          // encrypted && authenticated
static volatile badge_ble_state_t s_state = BADGE_BLE_UNPAIRED;
static volatile bool s_started;
static char s_passkey[8];                // 屏幕上显示的那 6 位；没在配对时为空
// 最近一次收到主机数据的时刻。链路层的沉默得由应用自己发现（见 badge_ble_idle_ms）。
static volatile uint32_t s_last_rx_ms;

// 入方向的分帧缓冲：GATT 写是切片来的，一行要跨好几次。
static char s_line[BADGE_BLE_LINE_MAX];
static size_t s_line_length;
static bool s_overflow;

static void set_state(badge_ble_state_t state)
{
    if (s_state != state) {
        ESP_LOGI(TAG, "状态: %d -> %d", (int)s_state, (int)state);
        s_state = state;
    }
}

static bool secured_now(void)
{
    struct ble_gap_conn_desc desc;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
        return false;
    }
    // 两个位都要：encrypted 只是「有加密」，authenticated 才是「配对时做过人证」。
    // 只要 Just Works 就能满足前者，而这条链路上那是不可接受的。
    return desc.sec_state.encrypted && desc.sec_state.authenticated;
}

// refresh_security 重新读一次安全状态，并在「刚变安全/刚变不安全」时更新对外的状态。
// --- 沉默检测 ----------------------------------------------------------------

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// touch_rx 记下「刚刚收到主机的东西」。连接建立时也必须调一次，否则 idle 会从开机
// 那一刻算起——连上就立刻被判成超时，连接刚建立就被自己掐断。
static void touch_rx(void)
{
    s_last_rx_ms = uptime_ms();
}

uint32_t badge_ble_idle_ms(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return 0U;
    }
    return uptime_ms() - s_last_rx_ms;
}

static void refresh_security(const char *when)
{
    const bool secured = secured_now();

    if (secured != s_secured) {
        ESP_LOGW(TAG, "%s：安全状态变为 %s", when, secured ? "已认证加密" : "不再是安全链路");
    }
    s_secured = secured;
    if (secured) {
        set_state(BADGE_BLE_SECURED);
        return;
    }
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        int bonds = 0;

        (void)ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &bonds);
        set_state(bonds > 0 ? BADGE_BLE_PAIRED : BADGE_BLE_UNPAIRED);
    }
}

// --- 入方向：按行分帧 ---------------------------------------------------------

static void deliver(void)
{
    size_t length = s_line_length;

    s_line_length = 0;
    if (s_overflow) {
        // 一句都不说是最坏的处理方式：上层只会看到「设备没反应」。
        ESP_LOGW(TAG, "丢弃超长的一行（上限 %u 字节）", (unsigned)BADGE_BLE_LINE_MAX);
        s_overflow = false;
        return;
    }
    if (length > 0U && s_line[length - 1U] == '\r') {
        --length;
    }
    if (length < BADGE_BLE_LINE_PREFIX_LENGTH ||
        strncmp(s_line, BADGE_BLE_LINE_PREFIX, BADGE_BLE_LINE_PREFIX_LENGTH) != 0) {
        return; // 不是给我们的
    }
    if (s_callback != NULL) {
        s_callback(s_line + BADGE_BLE_LINE_PREFIX_LENGTH,
                   length - BADGE_BLE_LINE_PREFIX_LENGTH, s_context);
    }
}

static void feed(const char *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        const char c = data[i];

        if (c == '\n') {
            deliver();
            continue;
        }
        if (s_line_length >= sizeof(s_line)) {
            // 一行超过了缓冲：丢掉它，但继续吃掉后续字节直到换行——不然后半个长行
            // 会被当成下一条消息的开头。
            s_overflow = true;
            continue;
        }
        s_line[s_line_length++] = c;
    }
}

// --- 出方向 ------------------------------------------------------------------

// notify_chunks 把一段字节按协商到的 MTU 逐个 notify 出去。
static bool notify_chunks(const char *data, size_t length)
{
    size_t sent = 0;

    while (sent < length) {
        size_t chunk = length - sent;

        if (chunk > s_chunk) {
            chunk = s_chunk;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + sent, (uint16_t)chunk);

        if (om == NULL || ble_gatts_notify_custom(s_conn_handle, s_tx_handle, om) != 0) {
            ESP_LOGW(TAG, "notify 失败（已发 %u/%u 字节）", (unsigned)sent, (unsigned)length);
            return false;
        }
        sent += chunk;
    }
    return true;
}

bool badge_ble_send(const char *data, size_t length)
{
    if (data == NULL || length == 0U) {
        return false;
    }
    // 安全之前一个字都不发：这条链路上的每一行都可能是在批准一条命令。
    if (!s_secured || !s_subscribed || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        // 三个值都得打出来：这个返回是静默的，而「发不出去、两侧又都不报错」在真机上
        // 让人只知道盯着屏幕猜（一次 22 字节的 sync 连续失败，日志里却什么都没有）。
        ESP_LOGW(TAG, "发送被拒：secured=%d subscribed=%d conn=0x%04x",
                 (int)s_secured, (int)s_subscribed, (unsigned)s_conn_handle);
        return false;
    }
    // 成帧与串口那条对称（见 badge_link_send）：收方是个**字节流**，行边界只能由发方给。
    // 省掉这个前缀与换行，主机就把每一行都当成控制台日志丢掉——而发方这边完全正常。
    return notify_chunks(BADGE_BLE_LINE_PREFIX, BADGE_BLE_LINE_PREFIX_LENGTH) &&
           notify_chunks(data, length) &&
           notify_chunks("\n", 1U);
}

// --- GATT --------------------------------------------------------------------

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        char buf[256];
        uint16_t length = 0;

        // 收到任何一段写都算「主机还在」——包括那段把它唤醒的裸换行。
        touch_rx();
        refresh_security("写入时");
        if (!s_secured) {
            // 属性层面的 AUTHEN 已经挡过一次，这里是第二道：真到了这一步说明配置被改坏了。
            ESP_LOGW(TAG, "明文写入被拒（attr=%u）", (unsigned)attr_handle);
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        while (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &length) == 0 && length > 0U) {
            // 一段写可能只取出一部分；feed 之后继续取，直到这一条写消息读完。
            feed(buf, length);
            if (length < sizeof(buf)) {
                break;
            }
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        refresh_security("读取时");
        return os_mbuf_append(ctxt->om, "", 0) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BADGE_BLE_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(BADGE_BLE_RX_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_AUTHEN |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BADGE_BLE_TX_UUID),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_AUTHEN |
                         BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

// --- 广播与事件 --------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg);

// subscribe_reason_name 把订阅事件的来源说清楚：对端写入 CCCD、从 NVS 恢复（bonded
// 重连）、还是链路结束（清除）。三者在日志里长得一样，但含义完全不同——上一版就是
// 因为分不清「恢复」与「写入」而在 CONNECT 里把恢复来的订阅清掉了。
static const char *subscribe_reason_name(uint8_t reason)
{
    switch (reason) {
    case BLE_GAP_SUBSCRIBE_REASON_WRITE:
        return "对端写入";
    case BLE_GAP_SUBSCRIBE_REASON_RESTORE:
        return "从 NVS 恢复";
    case BLE_GAP_SUBSCRIBE_REASON_TERM:
        return "链路结束";
    default:
        return "未知来源";
    }
}

static int advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params params = { 0 };

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)BADGE_BLE_NAME;
    fields.name_len = strlen(BADGE_BLE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);

    if (rc != 0) {
        return rc;
    }
    // 始终可连接：macOS 是 central，它需要看到广播才找得到我们（bond 只解决安全，
    // 不解决「谁来发起连接」）。
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            advertise();
            return 0;
        }
        s_conn_handle = event->connect.conn_handle;
        // ⚠️ 这里**不能**清 s_subscribed。bonded 对端重连时，NimBLE 在加密恢复的那一刻
        // 就从 NVS 恢复 CCCD 并发一条 reason=RESTORE 的订阅事件，而那条事件可能**先于**
        // 这个 CONNECT 事件到达（ble_gap.c 的 ble_gatts_bonding_restored）。清一下就等于
        // 把刚恢复的订阅扔掉：整条连接上每个字都被守卫静默拒掉，30 秒后还被自己的
        // 「主机沉默」规则断开——真机上正是这个症状，而且两侧都不报错。
        // 订阅由事件驱动：置位的是 SUBSCRIBE，清除的是 DISCONNECT（TERM）与「对端写入 0」。
        s_line_length = 0;
        s_overflow = false;
        // 连接本身算一次活动，否则 idle 会从开机那一刻算起——连上就立刻被判成超时。
        touch_rx();
        ESP_LOGI(TAG, "连上了（handle=%d）", (int)s_conn_handle);
        refresh_security("连接时");
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "断开 reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        s_secured = false;
        s_line_length = 0;
        s_passkey[0] = '\0';
        refresh_security("断开后");
        advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "加密状态变化 status=%d", event->enc_change.status);
        // 配对到此结束（成功或失败都一样），那 6 位码不能再留在屏幕上：配对屏是由
        // 「有码」驱动的，不清掉它界面就永远回不到状态屏——真机上撞到过，码一直挂到
        // 连接断开为止。失败也没关系：再配对会走一次 PASSKEY_ACTION，那时会生成新码。
        s_passkey[0] = '\0';
        refresh_security("加密后");
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        // 有屏没键盘：由我们自己生成 6 位码、显示、交回协议栈。
        // **每次配对都换一个**——常显的固定码等于贴了张纸条。
        {
            const unsigned code = esp_random() % 1000000U;
            struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = code };

            (void)snprintf(s_passkey, sizeof(s_passkey), "%06u", code);
            set_state(BADGE_BLE_PAIRING);
            ESP_LOGW(TAG, "配对码: %s（在电脑上输入它）", s_passkey);
            ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 对方重新要求配对（它那边把 bond 删了）。把本机这条陈旧 bond 丢掉再重配，
        // 否则就是那个经典死锁。
        ESP_LOGW(TAG, "对方要求重新配对，删掉本机的陈旧 bond");
        {
            struct ble_gap_conn_desc desc;

            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        s_subscribed = event->subscribe.cur_notify != 0;
        // reason 必须打出来：RESTORE（bonded 重连时从 NVS 恢复）与 WRITE（对端写 CCCD）
        // 看起来一样，但前者可能先于 CONNECT 到达，正是这一版踩过的坑。
        ESP_LOGI(TAG, "订阅: notify=%d（%s）", (int)s_subscribed,
                 subscribe_reason_name(event->subscribe.reason));
        return 0;

    case BLE_GAP_EVENT_MTU:
        // 出方向的切片大小跟着它走：协商到 256 时一次能发 253 字节。
        s_chunk = event->mtu.value > 3U ? (uint16_t)(event->mtu.value - 3U) : BADGE_BLE_DEFAULT_CHUNK;
        ESP_LOGI(TAG, "MTU=%d（每次发 %u 字节）", (int)event->mtu.value, (unsigned)s_chunk);
        return 0;

    default:
        return 0;
    }
}

// --- 对外 --------------------------------------------------------------------

badge_ble_state_t badge_ble_state(void)
{
    return s_state;
}

const char *badge_ble_passkey(void)
{
    return s_passkey;
}

void badge_ble_forget(void)
{
    // 「忘记那台电脑」：删掉所有 bond，重新广播。电脑那边还留着它的那份，所以它会
    // 「连上即断」——用户必须去系统设置里 Forget 那块 badge，这是唯一的补救路径
    // （CoreBluetooth 没有 unpair API）。
    ESP_LOGW(TAG, "忘记配对的电脑");
    (void)ble_store_util_delete_all(BLE_STORE_OBJ_TYPE_PEER_SEC, NULL);
    (void)ble_store_util_delete_all(BLE_STORE_OBJ_TYPE_OUR_SEC, NULL);
    s_passkey[0] = '\0';
    s_secured = false;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    set_state(BADGE_BLE_UNPAIRED);
}

void badge_ble_drop(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ESP_LOGW(TAG, "主机沉默了太久：主动断开，重新开始广播");
    // 断开事件（DISCONNECT）会负责清状态并重新广播，这里不必重复。
    (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "拿不到本机地址，BLE 链路不可用");
        return;
    }
    // TX 特征的句柄只有**到这里**才拿得到：add_svcs 只是把服务定义记下来，
    // ATT 属性表和句柄是 ble_gatts_start() —— 同步流程的一部分 —— 生成的，
    // 在那之前 find_chr 必然 ENOENT（症状：找不到 TX 特征，始终不广播）。
    const ble_uuid_t *svc_uuid = BLE_UUID16_DECLARE(BADGE_BLE_SVC_UUID);
    const ble_uuid_t *chr_uuid = BLE_UUID16_DECLARE(BADGE_BLE_TX_UUID);

    if (ble_gatts_find_chr(svc_uuid, chr_uuid, NULL, &s_tx_handle) != 0) {
        ESP_LOGE(TAG, "找不到 TX 特征，不广播（设备会看不见）");
        return;
    }
    refresh_security("同步后");
    if (s_state != BADGE_BLE_PAIRED && s_state != BADGE_BLE_SECURED) {
        set_state(BADGE_BLE_UNPAIRED);
    }
    ESP_LOGI(TAG, "开始广播：%s", BADGE_BLE_NAME);
    if (advertise() != 0) {
        ESP_LOGE(TAG, "广播启动失败");
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "协议栈复位 reason=%d", reason);
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_secured = false;
    s_subscribed = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t badge_ble_start(badge_ble_line_cb_t callback, void *context)
{
    if (s_started) {
        s_callback = callback;
        s_context = context;
        return ESP_OK;
    }

    s_callback = callback;
    s_context = context;

    esp_err_t err = nimble_port_init();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(err));
        return err;
    }

    // 有屏没键盘 → Passkey Entry；MITM + SC 是「不许 Just Works」这条要求的全部写法。
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
    if (ble_svc_gap_device_name_set(BADGE_BLE_NAME) != 0) {
        ESP_LOGE(TAG, "设备名设置失败");
        return ESP_FAIL;
    }

    int rc = ble_gatts_count_cfg(s_services);

    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_services);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 注册失败 rc=%d", rc);
        return ESP_FAIL;
    }

    nimble_port_freertos_init(host_task);
    s_started = true;
    return ESP_OK;
}
