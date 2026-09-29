// Tachi Badge —— 把 tachi 的「等一下」搬到这块小屏上的设备侧。
//
// 这是一个二次开发应用，不是基线 demo：它没有菜单，也不进任何 demo_* 测试页，
// 启动后直接进自己的界面（见 badge_ui）。协议的另一半在 tachi 仓库的
// desktop/link/link.go，一份设计说明在 docs/2026-09-28-agent-badge-link-design.md。
//
// 线程模型（这块板只有一个核，所以「谁碰什么」必须写清楚）：
//
//   link 任务   →  只读串口、按行分帧、把行丢进队列
//   按键回调    →  只把一次按键丢进队列（回调跑在共享定时器任务上，不能做重活）
//   app 任务    →  取行/取按键、跑状态机、发回答、持 LVGL 锁渲染
//
// 只有 app 任务碰状态机与界面，所以除了 LVGL 锁之外没有别的并发要说。
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "badge_ble.h"
#include "badge_link.h"
#include "badge_power.h"
#include "badge_proto.h"
#include "badge_sound.h"
#include "badge_state.h"
#include "badge_ui.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "lvgl.h"
#include "bsp_pins.h"

static const char *TAG = "badge";

#define BADGE_FIRMWARE "0.1.0"

#define BADGE_APP_STACK 8192U
#define BADGE_APP_PRIORITY 5U
#define BADGE_APP_TICK_MS 100U

// 一条入站协议行的上限。它必须容得下**一次问多个问题**的那一整行：三个问题、
// 每个带若干选项，实测能到 2KB 出头。原先定 1024，正好卡在「两个问题」的边缘——
// 超了是被整行丢掉的，而丢掉是静默的，于是设备上看起来像没收到。
//
// 与 badge_link 的 BADGE_LINK_LINE_MAX 保持一致：两处不一致时，瓶颈永远在小的那个，
// 而它藏在一个完全不同的文件里。
#define BADGE_LINE_MAX 4096U
// 队列深度 2 足够：app 任务每 100ms 就取走一条，而 4096 每条的内存代价不小。
#define BADGE_LINE_QUEUE_DEPTH 2U
#define BADGE_KEY_QUEUE_DEPTH 8U

#define BADGE_BATTERY_SAMPLE_MS 10000U
// 心跳：定期向主机要一次全量。它同时解决两件事——主机换了一次连接、或者我们
// 错过了什么，都能自己补回来；而「多久没收到东西」也就是链路是否还活着的判据。
#define BADGE_HEARTBEAT_MS 10000U
#define BADGE_LINK_TIMEOUT_MS 30000U
// 「忘记那台电脑」要点两次：第一次只是上膛（屏幕上说一句），第二次才真的忘掉。
// 这个窗口过后自动退膛——一个停留在「待确认」状态的设备比误触更糟。
#define BADGE_FORGET_CONFIRM_MS 6000U
// 按键路径等 LVGL 锁的上限。滚动要动 LVGL 对象，而锁通常只被一帧渲染短暂占着；
// 等不到时这次按键就不生效（再按一次即可）——比让一次「滚动」意外变成「换题」好。
#define BADGE_UI_LOCK_MS 100U

// 正常亮度。空闲熄屏就是把它降到 0（见 badge_power）——省电靠的是背光，不是让
// 芯片睡下去：睡下去就听不到主机递过来的 ask，而那是这块设备唯一的存在理由。
#define BADGE_BACKLIGHT_LEVEL 80U

typedef struct {
    char text[BADGE_LINE_MAX];
    size_t length;
} badge_line_t;

// 按键从回调到 app 任务只走一个值：按键回调跑在共享定时器任务上，那里不能
// 访问 LVGL，也不该做任何判断以外的事。
typedef struct {
    badge_key_t key;
} badge_key_event_t;

static QueueHandle_t s_line_queue;
static QueueHandle_t s_key_queue;

// 状态机与消息结构都有几 KB，任务栈放不下，放静态区。
static badge_state_t s_state;
static badge_msg_t s_message;
static badge_ui_snapshot_t s_snapshot;

static int s_battery_percent = -1;
static uint32_t s_last_message_ms;

// 上一次看到的 BLE 链路状态。蓝牙那一侧的状态是**异步**变的（配对码到了、加密
// 完成了，都发生在 NimBLE 的任务上），不盯着它，界面就永远不会因为「开始配对了」
// 而切到配对屏——那 6 位码也就没人看得见。
static badge_ble_state_t s_last_ble_state = BADGE_BLE_UNPAIRED;

// 屏幕亮灭（只动背光）。判据是纯逻辑，在 badge_power 里，主机测试覆盖它。
static badge_power_t s_power;

// set_backlight 是**唯一**写背光的地方：屏幕状态变了才写一次，
// 而不是每个 tick 都写一遍 LEDC。
static void apply_backlight(void)
{
    bsp_display_backlight(s_power.screen_on ? BADGE_BACKLIGHT_LEVEL : 0U);
    ESP_LOGI(TAG, "屏幕%s（待答 %u 条）", s_power.screen_on ? "亮" : "灭",
             (unsigned)s_state.ask_count);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_line(const char *line, size_t length, void *context)
{
    // ⚠ 必须 static：badge_line_t 里带着 BADGE_LINE_MAX（4096）字节的行缓冲，而本
    // 回调跑在 badge_link 任务上，那个任务的栈只有 4096 字节——放在栈上等于一个
    // 局部变量占满整个栈。表现是「主机一发消息，设备立刻 panic 重启」
    // （Guru Meditation: Stack protection fault），屏幕上则是一秒一闪的白屏：
    // 每次重启都会白一下。回调只由 badge_link 任务同步调用（链路是独占的），复用
    // 这一份缓冲是安全的。
    static badge_line_t item;

    (void)context;
    ESP_LOGD(TAG, "rx %u bytes: %.48s", (unsigned)length, line);
    if (length >= BADGE_LINE_MAX) {
        // 丢掉也要说一声：静默丢弃正是「设备看起来没收到」的成因，
        // 而排查的人手里只有一行「什么都没有」。
        ESP_LOGW(TAG, "rx dropped: %u bytes exceeds %u", (unsigned)length,
                 (unsigned)BADGE_LINE_MAX);
        return;
    }
    memcpy(item.text, line, length);
    item.text[length] = '\0';
    item.length = length;
    // 队列满就丢最新的：app 任务总会再向主机要一次全量（心跳），
    // 让队列里的旧消息排队等待的价值不大。
    (void)xQueueSend(s_line_queue, &item, 0);
}

static void on_key(bsp_btn_t button, bsp_btn_ev_t event, void *context)
{
    badge_key_event_t queued;

    (void)context;
    switch (button) {
    case BSP_BTN_UP:
        if (event == BSP_BTN_CLICK) {
            queued.key = BADGE_KEY_UP;
        } else if (event == BSP_BTN_LONG) {
            queued.key = BADGE_KEY_PREV;
        } else {
            return;
        }
        break;
    case BSP_BTN_DOWN:
        if (event == BSP_BTN_CLICK) {
            queued.key = BADGE_KEY_DOWN;
        } else if (event == BSP_BTN_LONG) {
            queued.key = BADGE_KEY_NEXT;
        } else {
            return;
        }
        break;
    case BSP_BTN_OK:
        if (event == BSP_BTN_CLICK) {
            queued.key = BADGE_KEY_OK;
        } else if (event == BSP_BTN_LONG) {
            queued.key = BADGE_KEY_SUBMIT;
        } else {
            return;
        }
        break;
    default:
        return;
    }
    (void)xQueueSend(s_key_queue, &queued, 0);
}

// 链路状态映射给界面。badge_state 只懂协议那一层（它不知道自己在跟谁说话），
// 所以「走的是哪条传输、安全了没有」只有这里知道，由这里喂进快照。
// active_transport 说出方向此刻走哪条链路。
//
// 这条判据只有这一份：send_line 按它路由，状态屏顶栏按它显示。两处各写一遍的话，
// 「屏幕上写着 BLE、消息却从串口出去」这种事只是时间问题——而这类不一致正是排查
// 时最费时间的那种。
//
// 判据本身来自两边的不对称：主机一次只用一条传输，BLE 只有在**已认证加密**时才算
// 可用（见 badge_ble），而串口那条一直「可用」——没主机时它的写会失败，无害。
static badge_ui_transport_t active_transport(void)
{
    return badge_ble_state() == BADGE_BLE_SECURED ? BADGE_UI_TRANSPORT_BLE
                                                  : BADGE_UI_TRANSPORT_USB;
}

static badge_ui_link_t link_for_ui(void)
{
    switch (badge_ble_state()) {
    case BADGE_BLE_PAIRING:
        return BADGE_UI_LINK_PAIRING;
    case BADGE_BLE_PAIRED:
        return BADGE_UI_LINK_PAIRED;
    case BADGE_BLE_SECURED:
        return BADGE_UI_LINK_SECURED;
    case BADGE_BLE_UNPAIRED:
    default:
        return BADGE_UI_LINK_UNPAIRED;
    }
}

static void render(void)
{
    badge_state_to_ui(&s_state, &s_snapshot);
    s_snapshot.battery_percent = s_battery_percent;
    // 界面据此决定「选中那一行要不要跑马灯」：熄屏时停掉动画。
    s_snapshot.screen_on = s_power.screen_on;
    // 配对码非空时界面会直接切到配对屏。它压过一切——那 6 位数字是用户此刻唯一
    // 的任务，而它只在配对进行中才有值。
    s_snapshot.link = link_for_ui();
    s_snapshot.passkey = badge_ble_passkey();
    // 顶栏那三个字母：和 send_line 用的是同一条判据（见 active_transport）。
    s_snapshot.transport = active_transport();

    // LVGL 不是线程安全的：这一屏的每一次修改都要在锁里。拿不到锁就跳过这一帧，
    // 下一轮再画——为了一帧画面去等，会把按键的响应一起拖住。
    ESP_LOGD(TAG, "render: view=%d connected=%d ask=%u sel=%u notice=%s",
             (int)s_snapshot.view, (int)s_snapshot.connected,
             (unsigned)s_state.ask_count, (unsigned)s_snapshot.selection,
             s_snapshot.notice != NULL ? s_snapshot.notice : "-");
    // 偶发地报一次 LVGL 池的状态。池耗尽或碎片严重时屏幕上会是白的，而设备本身
    // 还在正常收发协议——从外面看和「屏幕坏了」一模一样，所以留一条线索。默认
    // 等级下不可见，排查时把它调到 DEBUG 就能看到。
    {
        static uint32_t last_report;
        uint32_t now = now_ms();

        if (now - last_report >= 3000U) {
            lv_mem_monitor_t mon;

            last_report = now;
            lv_mem_monitor(&mon);
            ESP_LOGD(TAG, "lvgl: free=%u max_free=%u frag=%u%% used=%u%%",
                     (unsigned)mon.free_size, (unsigned)mon.free_biggest_size,
                     (unsigned)mon.frag_pct, (unsigned)mon.used_pct);
        }
    }
    if (!bsp_lvgl_lock(200)) {
        ESP_LOGW(TAG, "lvgl lock timeout — skipping this frame");
        return;
    }
    badge_ui_render(&s_snapshot);
    bsp_lvgl_unlock();
}

// 出方向的路由。注意开机那句 hello 一定走串口并丢掉——设备每 10 秒的 sync 会把
// 会话补回来。
static bool send_line(const char *text, size_t length)
{
    bool sent;

    if (length == 0U) {
        return false;
    }
    if (active_transport() == BADGE_UI_TRANSPORT_BLE) {
        sent = badge_ble_send(text, length);
    } else {
        sent = badge_link_send(text, length);
    }
    if (!sent) {
        ESP_LOGW(TAG, "tx failed (%u bytes)", (unsigned)length);
        return false;
    }
    // 发送也要留痕：排查「按了没反应」时，第一个要回答的问题就是「到底发出去了没」，
    // 而没有这行日志时它和「发出了但主机没答」看起来一模一样。
    ESP_LOGD(TAG, "tx %u bytes: %.64s", (unsigned)length, text);
    return true;
}

static void send_hello(void)
{
    char buffer[128];
    size_t length = badge_proto_hello(buffer, sizeof(buffer), BADGE_FIRMWARE);

    if (length > 0U) {
        (void)send_line(buffer, length);
    }
}

static void send_heartbeat(void)
{
    char buffer[64];
    size_t length = badge_proto_sync(buffer, sizeof(buffer), 0UL);

    if (length > 0U) {
        (void)send_line(buffer, length);
    }
}

static bool handle_line(const badge_line_t *line)
{
    bool was_connected = s_state.connected;

    if (!badge_proto_parse(line->text, line->length, &s_message)) {
        ESP_LOGW(TAG, "unparsed line: %.64s", line->text);
        return false; // 不是我们认识的消息：丢掉
    }
    s_last_message_ms = now_ms();
    if (s_message.kind == BADGE_MSG_RESET) {
        // 主机换了新的 ref 空间。屏幕会因此回到状态屏——这条日志是为了让它在现场有据
        // 可查，而不是让人以为「那条等待自己消失了」。
        ESP_LOGI(TAG, "主机重连：清空待答队列");
    }
    if (!was_connected) {
        // 第一次收到东西就是「连上了」——之后靠超时判断它是否还在。
        badge_state_set_connected(&s_state, true);
    }
    // 连接后的第一条消息也要 APPLY，不能只标记连接就返回。早先就是那么写的，
    // 于是重连后的第一条（可能正是一条 ask）被吞掉：屏幕停在「等电脑」，而主机
    // 那边看起来一切正常——它确实发出去了，设备也确实收到了。
    return badge_state_apply(&s_state, &s_message) || !was_connected;
}

// 状态屏上的长按确定 = 忘记配对的电脑。
//
// 为什么要按两次：这是个不可逆的动作，而且它**只做了一半**——本机忘掉之后，电脑
// 那边还留着它那一份 bond，于是之后会「连上即断」而且不会重新配对（macOS 的实测
// 行为）。一键完成的话，用户会在毫无提示的情况下撞上那个死锁，所以第一次按键
// 只是把这件事说出来，第二次才动手。
static uint32_t s_forget_deadline;

static void handle_forget(uint32_t now)
{
    if (s_forget_deadline == 0U || now >= s_forget_deadline) {
        s_forget_deadline = now + BADGE_FORGET_CONFIRM_MS;
        badge_state_notice(&s_state, "再长按一次确定：忘记电脑",
                           BADGE_FORGET_CONFIRM_MS);
        return;
    }
    s_forget_deadline = 0U;
    badge_ble_forget();
    badge_state_notice(&s_state, "已忘记：电脑上也要删", 6000U);
}

static void handle_key(const badge_key_event_t *event)
{
    // 静态而不是栈上：最坏情况接近 2KB，而任务栈只有 8192（见 BADGE_ANSWER_MAX）。
    static char payload[BADGE_ANSWER_MAX];
    size_t length = 0;

    // 待答屏上长按确定是「提交」；状态屏上它本来没有含义，用它做「忘记电脑」。
    // 判据用 ask_count 而不是「界面正在显示哪一屏」——状态机不知道界面在显示什么，
    // 而「没有等待项」恰好就是「它没停在待答屏上」。
    //
    // 配对进行中不算「状态屏」：屏幕上此刻是那 6 位码，而「忘记」会把这次配对直接
    // 搅黄——用户只是想把屏幕点亮，不该付出这个代价。
    if (s_state.ask_count == 0U && event->key == BADGE_KEY_SUBMIT) {
        if (badge_ble_state() != BADGE_BLE_PAIRING) {
            handle_forget(now_ms());
        }
        return;
    }
    // 长按上/下：正文装不下时先翻正文，翻到那一头才轮到状态机（换题）。判据在界面里
    // （「放不放得下」是布局事实），这里只负责把这次按键的归属问清楚。
    if (s_state.ask_count > 0U &&
        (event->key == BADGE_KEY_PREV || event->key == BADGE_KEY_NEXT)) {
        const int direction = (event->key == BADGE_KEY_NEXT) ? 1 : -1;
        bool scrolled = false;

        if (!bsp_lvgl_lock(BADGE_UI_LOCK_MS)) {
            ESP_LOGW(TAG, "lvgl lock timeout — 这次按键跳过");
            return;
        }
        scrolled = badge_ui_scroll(direction);
        bsp_lvgl_unlock();
        // 留痕：这一次长按归谁（翻屏还是换题）。排查时这是第一个要回答的问题——
        // 「按键到了、界面说翻不动」和「按键根本没到」在屏幕上看起来一模一样。
        ESP_LOGI(TAG, "长按翻屏: dir=%d scrolled=%d", direction, (int)scrolled);
        if (scrolled) {
            return;
        }
    }
    if (!badge_state_key(&s_state, event->key, payload, sizeof(payload), &length)) {
        return;
    }
    // 按下之后屏幕上必须有动静：这个答案要等主机回一条 ask_gone 才会撤下待答屏，
    // 中间那段时间里如果界面完全静止，用户会以为按键坏了。
    if (send_line(payload, length)) {
        badge_state_notice(&s_state, "已发送", 2500U);
    } else {
        badge_state_notice(&s_state, "发送失败", 4000U);
    }
}

static esp_err_t nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

static void badge_task(void *argument)
{
    uint32_t last_battery_ms = 0;
    uint32_t last_heartbeat_ms = 0;

    (void)argument;
    for (;;) {
        // 同 on_line：badge_line_t 是 4KB 级的结构，不该待在栈上。这个任务的栈
        // 是 8192，放得下，但剩下的余量要留给解析逻辑，不留这个隐患。
        static badge_line_t line;
        badge_key_event_t key;
        bool dirty = false;
        bool touched = false;
        uint32_t now = now_ms();

        if (xQueueReceive(s_line_queue, &line, 0) == pdTRUE) {
            const bool was_connected = s_state.connected;

            dirty = handle_line(&line) || dirty;
            // 只有「连上了 / 断了」算活动，消息本身不算：主机每 10 秒会回一条
            // 心跳式的 state，把它当活动的话空闲计时永远归零，屏幕根本不会灭。
            // 新的待答项不必在这里点亮——它由下面 badge_power_tick 的
            // has_pending_ask 负责，那也是同一个 tick 里更准确的判断。
            if (s_state.connected != was_connected) {
                touched = true;
            }
        }
        while (xQueueReceive(s_key_queue, &key, 0) == pdTRUE) {
            // 灭屏时的这一次按键只负责点亮，不进状态机：黑屏上你看不见光标停在
            // 哪一项、也看不见勾选状态，而三键里「确定」按下去就是一个决定
            // （可能是在批准一条要执行的命令）。
            if (s_power.screen_on) {
                handle_key(&key);
                dirty = true;
            }
            touched = true;
        }

        if (now - last_battery_ms >= BADGE_BATTERY_SAMPLE_MS) {
            int percent = bsp_battery_soc();

            last_battery_ms = now;
            if (percent != s_battery_percent) {
                s_battery_percent = percent < 0 ? -1 : percent;
                dirty = true;
            }
        }
        if (now - last_heartbeat_ms >= BADGE_HEARTBEAT_MS) {
            last_heartbeat_ms = now;
            send_heartbeat();
        }
        // 蓝牙链路状态变了必须重画：配对码到了要切到配对屏，配对完成要切回来。
        // 这个变化发生在 NimBLE 的任务上——不主动盯着它，界面就永远停在原处，
        // 而症状看起来只是「配对了但屏幕没反应」。
        {
            badge_ble_state_t ble = badge_ble_state();

            if (ble != s_last_ble_state) {
                s_last_ble_state = ble;
                dirty = true;
                // 配对是「现在需要你」的事，屏幕得亮着把码显示出来。
                touched = true;
            }
        }
        // 取时间之前必须重新读一次时钟：上面处理消息时刚把 s_last_message_ms 更新成
        // 「比 now 更晚」的时刻，用它去减会得到一个负数——uint32 下溢成 40 亿，
        // 于是每收到一条消息的同一轮就立刻被判成超时，连接状态永远是断的。
        now = now_ms();
        if (s_state.connected && now - s_last_message_ms >= BADGE_LINK_TIMEOUT_MS) {
            // 主机那边断了（拔线、tachi 关了）：屏幕上必须说清楚，否则「空闲」
            // 会被读成「agent 没在干活」。
            badge_state_set_connected(&s_state, false);
            dirty = true;
            touched = true; // 「等电脑」也是一件需要被看见的事，先点亮再说
        }
        // 蓝牙那条还得**真的把连接断掉**：链路层不知道对面已经没了，设备卡在
        // 「已连接」就永远不再广播——主机侧的症状是「设备不见了」，而屏幕上一切正常。
        //
        // 判据用的是 BLE 层自己记的「多久没收到数据」，而不是上面那个 connected：
        // 后者是「收到过一条完整协议行」才置位的，而「连上、订阅了、然后一个字节都
        // 没发」恰恰是最容易把设备卡住的处境，那时它还是 false。串口链路对此无感。
        if (badge_ble_idle_ms() >= BADGE_LINK_TIMEOUT_MS) {
            badge_ble_drop();
        }
        // 推进状态机自己的时钟：一次性提示（「已发送」）靠它过期。传 0 会让提示
        // 永远挂在那里——那是这个 tick 存在的唯一理由。
        if (badge_state_tick(&s_state, BADGE_APP_TICK_MS)) {
            dirty = true;
        }
        // 屏幕亮灭。touched 只在真有事件时置位（按键、链路状态变化），所以空闲计时
        // 不会被每 100ms 一次的 tick 自己清零；待答项则交给下面的 has_pending_ask。
        {
            bool flip = false;
            // 配对进行中也要保持亮屏：那 6 位码是用户此刻唯一要看的东西，屏幕在
            // 这时候熄灭等于把任务藏起来（而且他还在另一台设备上等着输）。
            bool keep_awake = s_state.ask_count > 0U ||
                              s_last_ble_state == BADGE_BLE_PAIRING;

            if (touched) {
                flip = badge_power_activity(&s_power, now);
            }
            if (badge_power_tick(&s_power, now, keep_awake)) {
                flip = true;
            }
            if (flip) {
                apply_backlight();
                // 屏幕状态变了要重画一次：熄灭时这一帧会把跑马灯停掉（黑屏背后的动画
                // 只是在烧电），亮起来时把选中那一行重新滚动起来。
                dirty = true;
            }
        }
        // 提示音：一条「该响一声」的新等待到了。判定是状态机做的（纯逻辑、有主机测试），
        // 这里只消费那一次性的标志并把它变成声音——响不响不在这里判断。
        if (s_state.alert_pending) {
            s_state.alert_pending = false;
            badge_sound_play();
        }
        if (dirty) {
            render();
        }
        vTaskDelay(pdMS_TO_TICKS(BADGE_APP_TICK_MS));
    }
}

void app_main(void)
{
    BaseType_t created;

    ESP_LOGI(TAG, "Tachi Badge %s starting", BADGE_FIRMWARE);

    if (nvs_init() != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed");
        return;
    }
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C initialization failed");
    }
    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "display/LVGL initialization failed");
        return;
    }
    badge_state_init(&s_state);
    badge_state_set_connected(&s_state, false);

    // 屏幕从亮开始；apply_backlight 是唯一写背光的地方。
    badge_power_init(&s_power, now_ms());
    apply_backlight();
    (void)bsp_battery_init();

    // 两条传输共用同一个「收到一行」入口：状态机不知道自己在跟谁说话。
    if (badge_ble_start(on_line, NULL) != ESP_OK) {
        // BLE 起不来不该拦住串口那条（反之亦然）：两条都是可选的。
        ESP_LOGW(TAG, "BLE 链路未能启动");
    }

    // 提示音：起播放任务并初始化 codec（初始化完就送去睡眠，只在响的时候唤醒）。
    // 同样地，它起不来只记一行日志——这条链路上的每一部分都是可选的。
    badge_sound_init();

    s_line_queue = xQueueCreate(BADGE_LINE_QUEUE_DEPTH, sizeof(badge_line_t));
    s_key_queue = xQueueCreate(BADGE_KEY_QUEUE_DEPTH, sizeof(badge_key_event_t));
    if (s_line_queue == NULL || s_key_queue == NULL) {
        ESP_LOGE(TAG, "queue creation failed");
        return;
    }

    // 字体自检：查两个字符能不能解析出字形描述符。字体表本身也可能「在 Flash 里
    // 但不认识字符」——那时屏幕上什么都不会画，从外面看和「屏幕坏了」一模一样。
    {
        lv_font_glyph_dsc_t dsc;

        ESP_LOGI(TAG, "font: line_height=%d base_line=%d subpx=%d",
                 (int)badge_font_16.line_height, (int)badge_font_16.base_line,
                 (int)badge_font_16.subpx);
        ESP_LOGI(TAG, "font: 'A' found=%d, U+4E2D(中) found=%d",
                 (int)lv_font_get_glyph_dsc(&badge_font_16, &dsc, 'A', 0),
                 (int)lv_font_get_glyph_dsc(&badge_font_16, &dsc, 0x4E2D, 0));
        // 光「查得到字形」不够：查得到而位图读错，屏幕上同样是空白。把字形自己的
        // 尺寸与偏移也打出来——box 为 0 说明描述符里的位图数据是空的或错位的。
        (void)lv_font_get_glyph_dsc(&badge_font_16, &dsc, 'A', 0);
        ESP_LOGI(TAG, "font: 'A' box=%ux%u ofs=%d,%d adv=%d",
                 (unsigned)dsc.box_w, (unsigned)dsc.box_h,
                 (int)dsc.ofs_x, (int)dsc.ofs_y, (int)dsc.adv_w);
        (void)lv_font_get_glyph_dsc(&badge_font_16, &dsc, 0x4E2D, 0);
        ESP_LOGI(TAG, "font: 中 box=%ux%u ofs=%d,%d adv=%d",
                 (unsigned)dsc.box_w, (unsigned)dsc.box_h,
                 (int)dsc.ofs_x, (int)dsc.ofs_y, (int)dsc.adv_w);
    }

    // 界面先立起来（状态屏），再打开链路：一块黑屏等连接比「等电脑」这句话更糟。
    if (bsp_lvgl_lock(1000)) {
        badge_ui_init();
        badge_state_to_ui(&s_state, &s_snapshot);
        s_snapshot.battery_percent = s_battery_percent;
        badge_ui_render(&s_snapshot);
        bsp_lvgl_unlock();
    }

    created = xTaskCreate(badge_task, "badge", BADGE_APP_STACK, NULL, BADGE_APP_PRIORITY, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return;
    }
    if (bsp_button_init(on_key, NULL) != ESP_OK) {
        // 按键不可用意味着回答不了任何问题，但界面仍然能显示——那种情况下
        // 用户至少能从屏幕上看到「有人在等」，然后回电脑处理。
        ESP_LOGW(TAG, "button initialization failed");
    }
    if (badge_link_start(on_line, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "link initialization failed");
        return;
    }
    send_hello();
}
