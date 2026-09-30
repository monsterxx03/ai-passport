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
#include "badge_voice.h"
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

// 上一次画过的「录音第几个十分之一秒」。倒计时是这一屏上唯一在动的数字，而每个 tick
// 都重画整个屏幕是纯浪费——它在十分位变化时才动（10 fps），而那正是人眼能看见的粒度。
static uint32_t s_voice_tenths;

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
        } else if (event == BSP_BTN_DOUBLE) {
            // 双击确定：把电脑上那个窗口换到前台。双击在三个键上都空着，而「它刚才说了
            // 什么」正是人离开电脑之后最想知道的那件事——内容在电脑上，这里只负责把窗口
            // 叫到眼前（见底栏提示）。
            queued.key = BADGE_KEY_ACTIVATE;
        } else if (event == BSP_BTN_PRESS) {
            // 按住说话：**按下**和**松开**都要送进队列（见 handle_key 的 TALK 分支）。
            // 缺了任何一个，录着的那一段就没人收尾。
            queued.key = BADGE_KEY_TALK_START;
        } else if (event == BSP_BTN_RELEASE) {
            queued.key = BADGE_KEY_TALK_END;
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
    // 录音进度（屏幕上的倒计时）。录了多久只有采集那条任务知道，所以由它报出来。
    s_snapshot.recording = badge_voice_progress(&s_snapshot.record_ms, &s_snapshot.record_limit_ms);

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

// badge_voice 只认「一行怎么发出去」，而那条路由只有 send_line 知道（BLE 还是串口）——
// 所以把它包一层交出去（badge_voice_init）。
static bool voice_send(const char *line, size_t length, void *context)
{
    (void)context;
    return send_line(line, length);
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

// 按住确定 = 说话（设计文档 §3 的首选）：按下开始录、**松开就发**，全程录着。两个事件
// 都走这里——按下被丢掉的话，录着的那一段就没有人来收尾了。
//
// 每一次按键都在屏幕上留一句话：录音跑在 badge_voice 自己的任务里，而状态机只由 app 任务
// 碰——没有这句话，用户按下去之后屏幕上是静止的，看起来就像按键坏了。太短的那些是例外
// （见下面那个 case）：双击确定（看账）就是两次短按，那里一冒字就会盖住用户真正在做的事。
static void handle_voice_key(bool pressed)
{
    const badge_voice_action_t action = pressed ? badge_voice_start() : badge_voice_finish();

    switch (action) {
    case BADGE_VOICE_ACTION_STARTED:
        badge_state_notice(&s_state, "录音中…松手发送", BADGE_VOICE_MAX_MS);
        break;
    case BADGE_VOICE_ACTION_STOPPED:
        // 结束时**不要**立刻说「已发送」：那一片片还在路上（实测 6 秒的语音要 3-4 秒传
        // 完），真正的结果由 badge_voice_take_report 在 app 循环里补上。
        badge_state_notice(&s_state, "发送中…", 20000U);
        break;
    case BADGE_VOICE_ACTION_BUSY:
        badge_state_notice(&s_state, "上一条还在发", 3000U);
        break;
    case BADGE_VOICE_ACTION_NO_AUDIO:
        badge_state_notice(&s_state, "音频起不来", 4000U);
        break;
    case BADGE_VOICE_ACTION_TOO_SHORT:
        // 太短的按住（双击那两下）不冒新字，但**要撤掉按下时那句**「录音中…松手发送」：
        // 松手这一下没有别人来接替它，不撤就会挂到那 30 秒的期限，看起来像还在录。
        badge_state_clear_notice(&s_state);
        break;
    case BADGE_VOICE_ACTION_NONE:
    default:
        // 松开时根本没在录（按下被别的屏吃掉了 / 录音已经因为到上限自己停了）：屏幕上
        // 那句话不是我们写的，别去动它。
        break;
    }
}

// 状态屏上的**长按上** = 忘记配对的电脑（长按确定让位给了「按住说话」，见 handle_key）。
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
        badge_state_notice(&s_state, "再长按一次上：忘记电脑",
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

    // 按住确定 = 说话（设计文档 §3 的首选，见 handle_voice_key）：**按下**那一下开始录，
    // **松开**那一下结束并发送。它排在最前面，因为按住这段横跨好几次按键，中途别的路由
    // 都该给它让路。
    //
    // 松开必须无条件走这条路：按下时允许录，中途来了一条待答项之后，松开这一下不能被
    // 「现在在待答屏上」挡掉——否则那一段就没人收尾，得等到 30 秒上限自己停。
    if (event->key == BADGE_KEY_TALK_END) {
        handle_voice_key(false);
        return;
    }
    if (event->key == BADGE_KEY_TALK_START) {
        // 待答屏上「确定」的意思是提交、配对时屏幕上是那 6 位码：这两种处境下按住确定
        // 不该顺手录走一段话（用户是在答题/配对，不是在说话）。
        if (s_state.ask_count == 0U && badge_ble_state() != BADGE_BLE_PAIRING) {
            handle_voice_key(true);
        }
        return;
    }

    // 录音中上键 = 丢弃这一段（说错了、手滑了）。它排在「长按上 = 忘记电脑」之前：录音
    // 时那一按的意思是「不要这段」，而不是「把电脑忘掉」。
    if (badge_voice_progress(NULL, NULL) &&
        (event->key == BADGE_KEY_UP || event->key == BADGE_KEY_PREV)) {
        badge_voice_cancel();
        badge_state_notice(&s_state, "已取消", 2500U);
        return;
    }

    // 状态屏（没有待答项）上的三个手势：它们遥控的是**电脑上那个窗口**。
    //
    // 设备不知道窗口在哪里、也不知道「翻一屏」是多大——它只把「你按了哪一下」报上去
    // （badge_proto_key），含义在主机那边（desktop/remote.go 的 Key）。
    //
    // 两条边界：
    //   - 只在状态屏上做。有人在等你回答时，这几个键的意思是**回答**（短上/短下在选项间
    //     移动、确定是提交，见 badge_state_key），不能因为电脑那边开着窗口就改掉。
    //   - 它排在「按住说话」和「录音中按上 = 丢弃这段」**之后**：那两件事横跨好几次按键，
    //     中途不能被别的意思吞掉（按下的那一下先到 TALK_START，松开才轮到这里的 CLICK）。
    if (s_state.ask_count == 0U) {
        const char *action = NULL;

        switch (event->key) {
        case BADGE_KEY_UP:
            action = "up";
            break;
        case BADGE_KEY_DOWN:
            action = "down";
            break;
        case BADGE_KEY_ACTIVATE:
            action = "activate";
            break;
        default:
            break;
        }
        if (action != NULL) {
            char line[32];
            const size_t length = badge_proto_key(line, sizeof(line), action);

            if (length > 0U) {
                (void)send_line(line, length);
            }
            return;
        }
    }

    // 主屏（没有等待项）上剩下的那个长按：
    //
    //   长按上 —— 忘记配对的电脑
    //
    // （「说话」不在这份名单里了：它是按下/松开这一对，见上面的 TALK 分支。）
    //
    // 配对进行中不做：屏幕上此刻是那 6 位码，用户可能只是想把屏幕点亮，不该因此把这次
    // 配对搅黄。
    if (s_state.ask_count == 0U && event->key == BADGE_KEY_PREV) {
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

        // 录音中每十分之一秒重画一次：倒计时不动的话，它看起来就像卡住了。
        {
            uint32_t elapsed = 0U;
            uint32_t limit = 0U;

            if (badge_voice_progress(&elapsed, &limit)) {
                const uint32_t tenths = elapsed / 100U;

                if (tenths != s_voice_tenths) {
                    s_voice_tenths = tenths;
                    dirty = true;
                }
            } else if (s_voice_tenths != 0U) {
                s_voice_tenths = 0U; // 录完了：下一次录音从 0 开始比
                dirty = true;
            }
        }

        // 一次录音的结果（采集与发送跑在 badge_voice 自己的任务里）。在这里显示而不在
        // 那边：状态机只由 app 任务碰。
        {
            badge_voice_report_t report;

            if (badge_voice_take_report(&report)) {
                char text[BADGE_DETAIL_MAX];
                bool show = true;

                switch (report.outcome) {
                case BADGE_VOICE_NO_AUDIO:
                    (void)snprintf(text, sizeof(text), "音频起不来");
                    break;
                case BADGE_VOICE_SEND_FAILED:
                    (void)snprintf(text, sizeof(text), "发送失败（链路不在？）");
                    break;
                case BADGE_VOICE_TOO_SHORT:
                    // 太短的「按住」静默收尾：双击确定（看账）就是两次短按，这里一冒字
                    // 就会盖住用户真正在做的那件事（见 badge_voice.h 的 BADGE_VOICE_MIN_MS）。
                    show = false;
                    break;
                case BADGE_VOICE_CANCELLED:
                    // 按键那条已经说过一次「已取消」，这里再来一句是给它一个确定的收尾
                    // （比如录到上限被丢弃时，用户看到的是这一句）。
                    (void)snprintf(text, sizeof(text), "已取消");
                    break;
                case BADGE_VOICE_OK:
                default:
                    // 说「录了几秒」而不说「发了几片」：片数是给主机对账用的（它按 end
                    // 里的 parts 能看出少了），而人要知道的是「刚才那句话上去了没有、
                    // 有多长」。丢了片要说出来——丢片的语音听起来像人吞了半句话。
                    if (report.peak < 200U) {
                        // 数字都对、但整段几乎没有波形：麦克风没通、增益为 0，或者只是
                        // 没人说话。这一句比「已发送」有用得多——它把「链路成功」和
                        // 「真的录到东西」分开了（第一次上真机时最常见的失败就在这）。
                        (void)snprintf(text, sizeof(text), "录到了 %u.%us，但没声音（峰值 %u）",
                                       (unsigned)(report.ms / 1000U),
                                       (unsigned)((report.ms % 1000U) / 100U),
                                       (unsigned)report.peak);
                    } else if (report.parts_failed > 0U) {
                        (void)snprintf(text, sizeof(text), "已发送 %u.%us（丢了 %u 片）",
                                       (unsigned)(report.ms / 1000U),
                                       (unsigned)((report.ms % 1000U) / 100U),
                                       report.parts_failed);
                    } else {
                        (void)snprintf(text, sizeof(text), "已发送 %u.%us 的语音",
                                       (unsigned)(report.ms / 1000U),
                                       (unsigned)((report.ms % 1000U) / 100U));
                    }
                    break;
                }
                if (show) {
                    badge_state_notice(&s_state, text, 6000U);
                    dirty = true;
                }
            }
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
        // 提示音：主机发来的一条 alert（有事等你 / 回合完成）。判定全在主机侧，这里只把
        // 那一次性的标志变成声音——响哪一种由状态机记着。放在电源判定**之前**：听见了
        // 却看不见，用户还得再按一下才看得到状态，那一声就白响了。
        if (s_state.alert_pending != BADGE_ALERT_NONE) {
            const badge_alert_kind_t kind = s_state.alert_pending;

            s_state.alert_pending = BADGE_ALERT_NONE;
            badge_sound_play(kind);
            touched = true;
        }
        // 屏幕亮灭。touched 只在真有事件时置位（按键、链路状态变化、提示音），所以空闲
        // 计时不会被每 100ms 一次的 tick 自己清零；待答项则交给下面的 has_pending_ask。
        {
            bool flip = false;
            // 配对进行中也要保持亮屏：那 6 位码是用户此刻唯一要看的东西，屏幕在
            // 这时候熄灭等于把任务藏起来（而且他还在另一台设备上等着输）。
            // 录音与发送中同理：屏幕上正显示「录音中 / 发送中」，而那正是手上这件事
            // 的进度——30 秒的录音足够长，撞上一次熄屏就是「我以为它没在录」。
            bool keep_awake = s_state.ask_count > 0U ||
                              s_last_ble_state == BADGE_BLE_PAIRING ||
                              badge_voice_busy();

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

    // 录音：只记住出口（一行怎么发出去），任务等按键时才起。要放在 badge_sound_init 之后
    // ——两者共用 codec，而初始化那一步是提示音那边做的（它先来）。
    badge_voice_init(voice_send, NULL);

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
