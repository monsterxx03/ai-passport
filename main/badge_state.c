#include "badge_state.h"

#include <stdio.h>
#include <string.h>

static void set_notice(badge_state_t *state, const char *text, uint32_t duration_ms)
{
    size_t length = strlen(text);

    if (length >= sizeof(state->notice)) {
        length = sizeof(state->notice) - 1U;
    }
    memcpy(state->notice, text, length);
    state->notice[length] = '\0';
    state->notice_deadline = state->now + duration_ms;
}

// reset_cursor 把待答屏的游标归零。队首换了一条待答项就必须做这件事：否则上一题
// 停在第三项，新题一上来光标就在第三项上，「确定」按下去答的是没看过的东西。
static void reset_cursor(badge_state_t *state)
{
    state->selection = 0;
    state->question_index = 0;
    memset(state->picked, 0, sizeof(state->picked));
}

void badge_state_init(badge_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

void badge_state_notice(badge_state_t *state, const char *text, uint32_t duration_ms)
{
    set_notice(state, text, duration_ms);
}

void badge_state_clear_notice(badge_state_t *state)
{
    state->notice[0] = '\0';
    state->notice_deadline = 0;
}

uint8_t badge_state_hint_phase(const badge_state_t *state)
{
    return (uint8_t)((state->now / BADGE_HINT_ROTATE_MS) % BADGE_HINT_COUNT);
}

bool badge_state_tick(badge_state_t *state, uint32_t elapsed_ms)
{
    const uint8_t phase_before = badge_state_hint_phase(state);
    bool changed = false;

    state->now += elapsed_ms;
    if (state->notice[0] != '\0' && state->now >= state->notice_deadline) {
        state->notice[0] = '\0';
        changed = true;
    }
    // 底栏那几条提示的轮换（见 badge_state_hint_phase）：翻一条也要报一次「变了」，否则
    // 那句话永远停在同一条上——渲染只在「变了」的时候发生（main.c 的 dirty）。
    // 注意这里是累加而不是提前 return：上面两条过期判定不能被这件事顶掉。
    if (badge_state_hint_phase(state) != phase_before) {
        changed = true;
    }
    return changed;
}

void badge_state_set_connected(badge_state_t *state, bool connected)
{
    state->connected = connected;
}

static bool drop_ask(badge_state_t *state, unsigned long ref)
{
    size_t i;

    for (i = 0; i < state->ask_count; ++i) {
        if (state->asks[i].ref != ref) {
            continue;
        }
        if (i + 1U < state->ask_count) {
            memmove(&state->asks[i], &state->asks[i + 1U],
                    (state->ask_count - i - 1U) * sizeof(state->asks[0]));
        }
        state->ask_count -= 1U;
        if (i == 0U) {
            reset_cursor(state);
        }
        return true;
    }
    return false;
}

// disclose_dropped_questions 把「有些题上不了屏」说出来。
//
// 题上不了屏有两种原因，对用户来说是同一件事：题数超过设备能放的（工具给模型宣称的
// 上限是 4），或者某道题的题面长到装不下——题面是回传答案的键，截断了模型会收到一个它
// 不认识的答案，所以那种题整道不显示。两种都不该静默：用户答完眼前这几题就提交，而模型
// 收到的是一个少了几条的答复，谁都不知道少了什么。
//
// 用一次性提示而不是常驻文案：它要说的是「剩下的到电脑上看」，而不是「等一下」。
static void disclose_dropped_questions(badge_state_t *state, const badge_msg_t *message)
{
    char text[BADGE_DETAIL_MAX];
    size_t dropped;

    if (message->ask_kind != BADGE_ASK_QUESTIONS ||
        message->questions_total <= message->question_count) {
        return;
    }
    dropped = message->questions_total - message->question_count;
    (void)snprintf(text, sizeof(text), "还有 %u 题没上屏，到电脑上答", (unsigned)dropped);
    set_notice(state, text, 5000U);
}

static bool push_ask(badge_state_t *state, const badge_msg_t *message)
{
    size_t i;

    // 同一条待答项的内容可能被主机重发（它的题面在流式阶段被改写）。用同样的
    // ref 落到原位，而不是当成新的一项——否则屏幕上会凭空多出一条。
    for (i = 0; i < state->ask_count; ++i) {
        if (state->asks[i].ref == message->ref) {
            // 主机每 10 秒的心跳会把这条 ask 重发一遍，所以「说一句」只在内容真的变了
            // 时才做——否则那条一次性的提示会每 10 秒弹一次，几乎常驻在底栏上，把
            // 按键提示挤掉。
            const bool changed = (state->asks[i].questions_total != message->questions_total) ||
                                 (state->asks[i].question_count != message->question_count);

            state->asks[i] = *message;
            if (changed) {
                disclose_dropped_questions(state, message);
            }
            return true;
        }
    }
    if (state->ask_count >= BADGE_MAX_ASKS) {
        // 装不下了。留在主机那边，等这里的处理完——主机每次同步都会重发它认为
        // 对端还没有的东西，所以丢掉不等于永远看不见。
        return false;
    }
    state->asks[state->ask_count] = *message;
    state->ask_count += 1U;
    if (state->ask_count == 1U) {
        reset_cursor(state);
    }
    disclose_dropped_questions(state, message);
    return true;
}

bool badge_state_apply(badge_state_t *state, const badge_msg_t *message)
{
    if (message == NULL) {
        return false;
    }
    switch (message->kind) {
    case BADGE_MSG_STATE: {
        bool changed = (state->session_count != message->session_count) ||
                       (state->sessions_total != message->sessions_total) ||
                       (memcmp(state->sessions, message->sessions,
                               sizeof(state->sessions)) != 0);

        state->session_count = message->session_count;
        state->sessions_total = message->sessions_total;
        memcpy(state->sessions, message->sessions, sizeof(state->sessions));
        return changed;
    }
    case BADGE_MSG_ASK:
        return push_ask(state, message);
    case BADGE_MSG_ASK_GONE:
        return drop_ask(state, message->ref);
    case BADGE_MSG_RESET:
        // 主机换了新的 ref 空间（它重启了，或者链路重连了——ref 是每条连接分配的）：
        // 我们手里这些旧 ref 再也等不到答案，而留着它们只会显示一条按下去没有反应的
        // 等待，屏幕还会把它当成队首。全清，等主机随后的全量重发。
        if (state->ask_count == 0U) {
            return false;
        }
        state->ask_count = 0U;
        reset_cursor(state);
        return true;
    case BADGE_MSG_ERROR:
        // 主机的拒绝要给用户看见：不然按下去没反应，看起来像是设备坏了。
        set_notice(state, message->message[0] != '\0' ? message->message : "主机拒绝了这次回答",
                   5000U);
        return true;
    case BADGE_MSG_VOICE_TEXT: {
        // 刚才那段语音转成了什么。它进的是状态屏那一行一次性提示：这是**回执**，不是
        // 状态——状态屏上长期挂着的东西是「谁在等你」。
        //
        // 「排队中」与「已发送」分开说：前者意味着这句话还没人看到（会话在跑，它在等一个
        // steer 点），而那时用户最需要知道的就是这个。
        char line[BADGE_DETAIL_MAX + BADGE_META_MAX];

        (void)snprintf(line, sizeof(line), "%s%s",
                       message->voice_state == BADGE_VOICE_QUEUED ? "排队中：" : "已发送：",
                       message->voice_text);
        set_notice(state, line, 8000U);
        return true;
    }
    case BADGE_MSG_ALERT:
        // 主机说「响一声」：它知道自己在不在前台、用户正在看哪个会话，而这块屏上
        // 一条信息都没有。这里只记下是哪一种，放音频是调用方的事（见 badge_sound）。
        //
        // 来的比放掉得快时，后面那条盖掉前面那条：连着两声的意义是零，而漏掉最后
        // 那条才是真会让人错过东西。
        state->alert_pending = message->alert_kind;
        return true;
    default:
        return false;
    }
}

static void move_selection(badge_state_t *state, int delta, size_t count)
{
    if (count == 0U) {
        state->selection = 0;
        return;
    }
    if (delta < 0) {
        state->selection = state->selection == 0U ? count - 1U : state->selection - 1U;
    } else {
        state->selection = (state->selection + 1U) % count;
    }
}

// append_label 把标签拼进答案串。多选的答案由调用方用 ", " 拼好——这与 TUI /
// desktop 的约定一致，模型看到的是同一个形状。
static void append_label(char *buffer, size_t cap, const char *label)
{
    size_t used = strlen(buffer);
    const char *separator = used > 0U ? ", " : "";

    if (used + strlen(separator) + strlen(label) + 1U > cap) {
        return; // 装不下就到此为止：宁可少一项，也不要写出半个字
    }
    (void)strcat(buffer, separator);
    (void)strcat(buffer, label);
}

static bool submit_questions(badge_state_t *state, const badge_msg_t *ask, char *out,
                             size_t cap, size_t *out_length)
{
    const char *keys[BADGE_MAX_QUESTIONS];
    const char *values[BADGE_MAX_QUESTIONS];
    char joined[BADGE_MAX_QUESTIONS][BADGE_TITLE_MAX * 2];
    size_t count = 0;
    size_t question;

    for (question = 0; question < ask->question_count; ++question) {
        const badge_question_t *item = &ask->questions[question];
        size_t option;

        joined[question][0] = '\0';
        for (option = 0; option < item->option_count; ++option) {
            if ((state->picked[question] & (uint8_t)(1U << option)) == 0U) {
                continue;
            }
            append_label(joined[question], sizeof(joined[question]),
                         item->options[option].label);
        }
        if (joined[question][0] == '\0') {
            // 有一题没答就不提交：answers 里少一个键，模型会收到一个残缺的答复，
            // 而它无从知道那一题是没人答还是被跳过了。
            set_notice(state, "还有问题没有作答", 3000U);
            return false;
        }
        keys[question] = item->question;
        values[question] = joined[question];
        count += 1U;
    }

    *out_length = badge_proto_answer_questions(out, cap, ask->ref, keys, values, count);
    if (*out_length == 0U) {
        // 答案编码不出来（题面太长或太多，装不下调用方的缓冲）。必须说一句：屏幕上
        // 「按了没反应」是这块板上最贵的一类故障，而它连日志都没有——用户只会以为
        // 按键坏了，然后一遍遍地按。
        set_notice(state, "答案装不下，到电脑上答", 5000U);
        return false;
    }
    return true;
}

static bool key_permission(badge_state_t *state, const badge_msg_t *ask, badge_key_t key,
                           char *out, size_t cap, size_t *out_length)
{
    switch (key) {
    case BADGE_KEY_UP:
        move_selection(state, -1, ask->option_count);
        return false;
    case BADGE_KEY_DOWN:
        move_selection(state, 1, ask->option_count);
        return false;
    case BADGE_KEY_OK:
        // 决定原样取自选项的机器值——设备不知道 allow_once 是什么意思，也不该知道。
        *out_length = badge_proto_answer_permission(out, cap, ask->ref,
                                                    ask->options[state->selection].value);
        return *out_length > 0U;
    case BADGE_KEY_SUBMIT:
    default:
        // 一条要放行的命令没有「忽略」这个选项：不回答它就一直是没回答。
        // 换题与提交在这里也没有意义：待答项只有一条，选项就那三个。
        return false;
    }
}

static bool key_questions(badge_state_t *state, const badge_msg_t *ask, badge_key_t key,
                          char *out, size_t cap, size_t *out_length)
{
    const badge_question_t *item = &ask->questions[state->question_index];

    switch (key) {
    case BADGE_KEY_UP:
        move_selection(state, -1, item->option_count);
        return false;
    case BADGE_KEY_DOWN:
        move_selection(state, 1, item->option_count);
        return false;
    case BADGE_KEY_PREV:
        // 回到上一题。第一题上它什么都不做——没有「上一题」可比回到。
        if (state->question_index > 0U) {
            state->question_index -= 1U;
            state->selection = 0;
        }
        return false;
    case BADGE_KEY_NEXT:
        // 去下一题。多选题没有别的办法离开当前题：短按确定被「勾选」占着，
        // 而长按确定是提交——所以换题必须由这两个键来承担（见 badge_state.h）。
        if (state->question_index + 1U < ask->question_count) {
            state->question_index += 1U;
            state->selection = 0;
        }
        return false;
    case BADGE_KEY_SUBMIT:
        // 两种题型都是长按提交：单选在末题按确定就自动提交，这里只是给它一条
        // 一样的手势；没答完的题会由 submit_questions 拒绝并给出提示。
        return submit_questions(state, ask, out, cap, out_length);
    case BADGE_KEY_OK:
    default:
        break;
    }

    if (item->multi_select) {
        state->picked[state->question_index] ^= (uint8_t)(1U << state->selection);
        return false;
    }
    // 单选：按下就是选中，然后走下一题或提交。单选的题不存在「没选中」的中间态，
    // 所以这里不会出现 submit 被拒绝的情况。
    state->picked[state->question_index] = (uint8_t)(1U << state->selection);
    if (state->question_index + 1U < ask->question_count) {
        state->question_index += 1U;
        state->selection = 0;
        return false;
    }
    return submit_questions(state, ask, out, cap, out_length);
}

bool badge_state_key(badge_state_t *state, badge_key_t key, char *out, size_t cap,
                     size_t *out_length)
{
    const badge_msg_t *ask;

    if (out_length != NULL) {
        *out_length = 0U;
    }
    if (out != NULL && cap > 0U) {
        out[0] = '\0';
    }
    if (state->ask_count == 0U || out == NULL || out_length == NULL) {
        // 状态屏上三个键都没有意义：那里没有可做的决定。
        return false;
    }

    ask = &state->asks[0];
    if (ask->ask_kind == BADGE_ASK_QUESTIONS) {
        return key_questions(state, ask, key, out, cap, out_length);
    }
    return key_permission(state, ask, key, out, cap, out_length);
}

void badge_state_to_ui(const badge_state_t *state, badge_ui_snapshot_t *snapshot)
{
    const badge_session_t *shown = NULL;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->connected = state->connected;
    snapshot->notice = state->notice[0] != '\0' ? state->notice : NULL;
    snapshot->session_count = state->session_count;
    snapshot->session_total = state->sessions_total;
    snapshot->battery_percent = -1; // 由调用方用 BSP 读到的值覆盖

    // 正在等你的那个会话优先：状态屏上最有用的信息是「谁需要我」，
    // 而不是「哪个 id 排在前」。
    if (state->ask_count > 0U) {
        size_t i;

        for (i = 0; i < state->session_count; ++i) {
            if (strcmp(state->sessions[i].id, state->asks[0].session) == 0) {
                shown = &state->sessions[i];
                break;
            }
        }
    }
    if (shown == NULL && state->session_count > 0U) {
        shown = &state->sessions[0];
    }
    if (shown != NULL) {
        snapshot->has_session = true;
        (void)memcpy(snapshot->session_title, shown->title, sizeof(snapshot->session_title));
        (void)memcpy(snapshot->state_label, shown->label, sizeof(snapshot->state_label));
        (void)memcpy(snapshot->state_detail, shown->detail, sizeof(snapshot->state_detail));
        snapshot->tool_calls = shown->tools;
    }

    if (state->ask_count > 0U) {
        snapshot->ask = &state->asks[0];
        snapshot->selection = state->selection;
        snapshot->question_index = state->question_index;
        snapshot->checked = state->picked[state->question_index];
    }
    // 只有两屏：有事等你（待答屏）和平时（状态屏）。
    snapshot->view = (state->ask_count > 0U) ? BADGE_UI_ASK : BADGE_UI_STATUS;
    // 底栏那几条提示该显示哪一条。放在这里而不是界面里：相位是时间推出来的（状态机的
    // 时钟），界面只读它——它不该自己养一个计时器。
    snapshot->hint_phase = badge_state_hint_phase(state);
}
