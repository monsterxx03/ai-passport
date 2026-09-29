// badge_state —— 设备侧的状态机：收到什么、键按下时该做什么。
//
// 它与 ESP-IDF 和 LVGL 都无关，只依赖协议结构（tests/test_badge_state.c 完整覆盖
// 它）。这样做的理由很直接：这块板上最贵的一类 bug 是「按键按下去没反应」和
// 「答回去的答案对不上题」——两者都能在主机上试出来，不必每次都烧板子。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "badge_proto.h"
#include "badge_ui.h"

// 同时挂着的待答项上限。一个会话一次只等一件事，所以这个数就是「几个会话同时在
// 等我」；超出部分留在主机那边，等这里的处理完由主机重发。
#define BADGE_MAX_ASKS 4

typedef enum {
    BADGE_KEY_UP = 0, // 短按上/下：在选项间移动
    BADGE_KEY_DOWN,
    BADGE_KEY_PREV,   // 长按上：上一题（第一题上无动作）
    BADGE_KEY_NEXT,   // 长按下：下一题（最后一题上无动作）
    BADGE_KEY_OK,     // 短按确定：单选=选中并前进；多选=勾选/取消勾选
    BADGE_KEY_SUBMIT, // 长按确定：提交。换题走 PREV/NEXT，这样一套手势对两种题型都成立
    BADGE_KEY_INFO,   // 双击确定：看一眼当前会话的账（模型、上下文、花费）
} badge_key_t;

typedef struct {
    bool connected;

    // 会话状态（状态屏）
    size_t session_count;
    badge_session_t sessions[BADGE_MAX_SESSIONS];
    // 主机报了多少个会话（≥ session_count）。底栏的「另有 N 个会话在跑」按它算，
    // 否则放不下的那些会让那句话少报。
    size_t sessions_total;

    // 待答队列。一次只显示队首，答完出队。
    size_t ask_count;
    badge_msg_t asks[BADGE_MAX_ASKS];

    // 待答屏上的游标
    size_t selection;
    size_t question_index;
    uint8_t picked[BADGE_MAX_QUESTIONS]; // 每题的选中位掩码（单选也用它的一位）

    // 一次性提示。notice_until 是一个单调递增的计数，由调用方推进
    // （badge_state_tick），到点自动清掉——提示不该永远挂在那里。
    char notice[BADGE_DETAIL_MAX];
    uint32_t notice_deadline;
    uint32_t now;

    // 有没有一声提示音还没被放掉，以及是哪一种（BADGE_ALERT_NONE = 没有）。一次性：
    // 由调用方（main 的循环）消费，它决定放哪段音频（见 badge_sound）。放在这里而不是
    // 直接放声音，是因为「什么时候该响、响哪一种」是**状态**（主机发来的 alert 消息），
    // 而状态机是纯逻辑、能在主机上测。
    badge_alert_kind_t alert_pending;

    // 会话信息屏那几行。它是一眼的东西，不是要停在那里的状态：所以有截止时间，到点自己
    // 回主屏（见 badge_state_tick）。数据由主机填（双击之后回一条 info）。
    badge_info_t info;
    bool info_open;
    uint32_t info_deadline;
} badge_state_t;

void badge_state_init(badge_state_t *state);

// badge_state_tick 推进内部时钟并让过期提示消失。返回 true 表示状态变了（需要重绘）。
bool badge_state_tick(badge_state_t *state, uint32_t elapsed_ms);

void badge_state_set_connected(badge_state_t *state, bool connected);

// badge_state_apply 吃一条协议消息。返回 true 表示视图变了。
bool badge_state_apply(badge_state_t *state, const badge_msg_t *message);

// badge_state_notice 显示一行一次性提示（比如「已发送」）。它自己会过期——
// 按下一个键之后界面上必须有动静，否则用户只能靠猜。
void badge_state_notice(badge_state_t *state, const char *text, uint32_t duration_ms);

// badge_state_info_open 打开会话信息屏（双击确定时调）。此刻数据可能还没到——那一屏
// 会先显示一句「读取中…」，回执到了再填上（见 badge_state_apply 的 BADGE_MSG_INFO）。
void badge_state_info_open(badge_state_t *state);

// badge_state_info_close 收起它。任何一次按键都该调它：那是一眼的东西，看完就走。
void badge_state_info_close(badge_state_t *state);

// badge_state_info_visible 它此刻开着吗（按键路径据此决定「这一下是收起它还是干别的」）。
bool badge_state_info_visible(const badge_state_t *state);

// badge_state_key 处理一次按键。
//
// 返回 true 表示**有东西要发**：out 里是一条完整的协议行（不含前缀与换行），
// 调用方负责送出去。送失败时调用方回滚不了状态，所以这里的动作设计成幂等：
// 同一条回答再按一次只会重发同样的一行。
bool badge_state_key(badge_state_t *state, badge_key_t key, char *out, size_t cap,
                     size_t *out_length);

void badge_state_to_ui(const badge_state_t *state, badge_ui_snapshot_t *snapshot);
