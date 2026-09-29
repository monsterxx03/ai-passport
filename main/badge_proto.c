#include "badge_proto.h"

#include <stdio.h>
#include <string.h>

#include "badge_json.h"

// take_string 取一个字符串字段。
//
// 返回 false 表示**被截断了**（字段缺失不算）。有些字段截断无所谓——标题短一点
// 照样看得懂——有些字段截断了语义就变了：AskUser 的问题全文是回传答案的键，
// 截一半等于答非所问，而且设备上看起来一切正常。调用方按字段自己决定怎么办。
static bool take_string(const bjson_val_t *obj, const char *key, char *dst, size_t cap)
{
    bjson_val_t value;
    size_t full = 0;
    size_t written;

    if (cap == 0U) {
        return true;
    }
    dst[0] = '\0';
    if (!bjson_obj_get(obj, key, &value)) {
        return true;
    }
    written = bjson_str_into(&value, dst, cap, &full);
    return written == full;
}

static unsigned long take_ulong(const bjson_val_t *obj, const char *key)
{
    bjson_val_t value;
    long number;

    if (!bjson_obj_get(obj, key, &value)) {
        return 0UL;
    }
    number = bjson_int(&value, 0L);
    return number < 0L ? 0UL : (unsigned long)number;
}

// take_bool 取一个布尔字段。缺字段、或者字段不是布尔，都按 fallback 处理——bjson_bool
// 自己就带这一层（见 badge_json.h）。
static bool take_bool(const bjson_val_t *obj, const char *key, bool fallback)
{
    bjson_val_t value;

    if (!bjson_obj_get(obj, key, &value)) {
        return fallback;
    }
    return bjson_bool(&value, fallback);
}

static void take_options(const bjson_val_t *owner, badge_option_t *options, size_t *count,
                         size_t max, size_t *total)
{
    bjson_val_t array;
    size_t length;
    size_t keep;
    size_t i;

    *count = 0;
    *total = 0;
    if (!bjson_obj_get(owner, "options", &array) || array.kind != BJSON_ARRAY) {
        return;
    }
    length = bjson_arr_len(&array);
    *total = length;

    // 放不下时**留最后一行说真话**：屏幕上没有地方同时放「5 个选项」和「还有 N 项」，
    // 而两者相比说谎更糟——用户看不到的选项就等于不存在，他无从知道少了什么。
    keep = length > max ? max - 1U : length;
    for (i = 0; i < length && *count < keep; ++i) {
        bjson_val_t item;
        badge_option_t *option = &options[*count];

        if (!bjson_arr_get(&array, i, &item) || item.kind != BJSON_OBJECT) {
            continue;
        }
        (void)take_string(&item, "label", option->label, sizeof(option->label));
        (void)take_string(&item, "value", option->value, sizeof(option->value));
        // 屏幕上是一行空白的项按不动；没有文字的项连按什么都不该发。
        if (option->label[0] == '\0') {
            continue;
        }
        // AskUser 的选项只有文字（tachi 侧的 tools.QuestionOption 就是 label +
        // description），没有独立的机器值——那时答案就是选项文字本身。
        if (option->value[0] == '\0') {
            memcpy(option->value, option->label, sizeof(option->value));
        }
        *count += 1U;
    }
}

static void take_questions(const bjson_val_t *root, badge_msg_t *out)
{
    bjson_val_t array;
    size_t length;
    size_t i;

    if (!bjson_obj_get(root, "questions", &array) || array.kind != BJSON_ARRAY) {
        return;
    }
    length = bjson_arr_len(&array);
    out->questions_total = length;
    if (length > BADGE_MAX_QUESTIONS) {
        length = BADGE_MAX_QUESTIONS;
    }
    for (i = 0; i < length; ++i) {
        bjson_val_t item;
        badge_question_t *question = &out->questions[out->question_count];
        bool complete;

        if (!bjson_arr_get(&array, i, &item) || item.kind != BJSON_OBJECT) {
            continue;
        }
        memset(question, 0, sizeof(*question));
        complete = take_string(&item, "question", question->question,
                               sizeof(question->question));
        (void)take_string(&item, "header", question->header, sizeof(question->header));
        {
            bjson_val_t multi;

            if (bjson_obj_get(&item, "multi_select", &multi)) {
                question->multi_select = bjson_bool(&multi, false);
            }
        }
        take_options(&item, question->options, &question->option_count, BADGE_MAX_OPTIONS,
                     &question->options_total);

        // 题面被截断，或者压根没有题面：这道题答不了。它不是「少一个字段」——
        // 答案要靠题面全文当键，键错了模型会收到一个它不认识的答案。宁可不上屏，
        // 让用户在电脑上回答；半道题比没有题更坏。
        if (!complete || question->question[0] == '\0') {
            continue;
        }
        // 没有选项的题（自由输入）**照样收下**：设备答不了它，但「有人在等你」
        // 这件事必须让人知道——丢掉它等于把一次等待变成静默，而那与这个设备
        // 存在的理由正好相反。界面会把这类题显示成「需要在电脑上回答」。
        out->question_count += 1U;
    }
}

static bool parse_state(const bjson_val_t *root, badge_msg_t *out)
{
    bjson_val_t sessions;
    size_t length;
    size_t i;

    out->kind = BADGE_MSG_STATE;
    // 没有会话是一个合法的状态（桌面上什么都没在跑），不是坏消息。
    if (!bjson_obj_get(root, "sessions", &sessions) || sessions.kind != BJSON_ARRAY) {
        return true;
    }
    length = bjson_arr_len(&sessions);
    out->sessions_total = length;
    if (length > BADGE_MAX_SESSIONS) {
        length = BADGE_MAX_SESSIONS;
    }
    for (i = 0; i < length; ++i) {
        bjson_val_t item;
        badge_session_t *session = &out->sessions[out->session_count];

        if (!bjson_arr_get(&sessions, i, &item) || item.kind != BJSON_OBJECT) {
            continue;
        }
        memset(session, 0, sizeof(*session));
        (void)take_string(&item, "id", session->id, sizeof(session->id));
        (void)take_string(&item, "title", session->title, sizeof(session->title));
        (void)take_string(&item, "status", session->status, sizeof(session->status));
        (void)take_string(&item, "label", session->label, sizeof(session->label));
        (void)take_string(&item, "detail", session->detail, sizeof(session->detail));
        session->tools = take_ulong(&item, "tools");
        out->session_count += 1U;
    }
    return true;
}

static bool parse_ask(const bjson_val_t *root, badge_msg_t *out)
{
    char kind[BADGE_META_MAX];

    out->kind = BADGE_MSG_ASK;
    out->ref = take_ulong(root, "ref");
    // 没有 ref 就答不回去：这条消息对设备毫无用处，早点丢掉比上屏后按不动好。
    if (out->ref == 0UL) {
        return false;
    }

    kind[0] = '\0';
    {
        bjson_val_t value;

        if (bjson_obj_get(root, "kind", &value)) {
            (void)bjson_str_into(&value, kind, sizeof(kind), NULL);
        }
    }
    (void)take_string(root, "title", out->title, sizeof(out->title));
    (void)take_string(root, "session", out->session, sizeof(out->session));

    if (strcmp(kind, "ask_user") == 0) {
        out->ask_kind = BADGE_ASK_QUESTIONS;
        take_questions(root, out);
        // 一道题都没上得了屏（题面都太长，或者压根没有题面）时**仍然收下**：主机那边
        // 正等一个答案，而屏幕必须让「有人在等你」这件事看得见——看不到等待，这块设备
        // 就不存在了。界面会把这种 ask 显示成「这题要到电脑上答」（见 badge_ui）。
        return out->question_count > 0U || out->questions_total > 0U;
    }

    out->ask_kind = BADGE_ASK_PERMISSION;
    (void)take_string(root, "body", out->body, sizeof(out->body));
    take_options(root, out->options, &out->option_count, BADGE_MAX_OPTIONS, &out->options_total);
    return out->option_count > 0U;
}

bool badge_proto_parse(const char *line, size_t length, badge_msg_t *out)
{
    bjson_t json = {.buf = line, .len = length};
    bjson_val_t root;
    bjson_val_t value;
    char kind[BADGE_META_MAX];

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (line == NULL || !bjson_parse(&json, &root) || root.kind != BJSON_OBJECT) {
        return false;
    }

    kind[0] = '\0';
    if (!bjson_obj_get(&root, "t", &value) || value.kind != BJSON_STRING) {
        return false;
    }
    (void)bjson_str_into(&value, kind, sizeof(kind), NULL);
    out->seq = take_ulong(&root, "seq");

    if (strcmp(kind, "state") == 0) {
        return parse_state(&root, out);
    }
    if (strcmp(kind, "ask") == 0) {
        return parse_ask(&root, out);
    }
    if (strcmp(kind, "ask_gone") == 0) {
        out->kind = BADGE_MSG_ASK_GONE;
        out->ref = take_ulong(&root, "ref");
        return out->ref != 0UL;
    }
    if (strcmp(kind, "error") == 0) {
        out->kind = BADGE_MSG_ERROR;
        out->ref = take_ulong(&root, "ref");
        (void)take_string(&root, "message", out->message, sizeof(out->message));
        return true;
    }
    if (strcmp(kind, "reset") == 0) {
        // 主机换了新的 ref 空间——每条连接都会重新分配 ref，所以旧的等待项在这条
        // 连接上再也等不到答案了。设备收到就把队列清空，等主机随后的全量重发。
        out->kind = BADGE_MSG_RESET;
        return true;
    }
    if (strcmp(kind, "alert") == 0) {
        // 响一声。kind 决定放哪段音频（见 badge_sound）；不认识的 kind 直接丢掉——
        // 放错一段声音比不响更让人困惑。
        char which[BADGE_META_MAX];

        which[0] = '\0';
        (void)take_string(&root, "kind", which, sizeof(which));
        if (strcmp(which, "ask") == 0) {
            out->kind = BADGE_MSG_ALERT;
            out->alert_kind = BADGE_ALERT_ASK;
            return true;
        }
        if (strcmp(which, "done") == 0) {
            out->kind = BADGE_MSG_ALERT;
            out->alert_kind = BADGE_ALERT_DONE;
            return true;
        }
        return false;
    }
    if (strcmp(kind, "hud") == 0) {
        // 桌面上那块置顶小窗的状态。设备只读它，不当它是内容——小窗里写的是什么，
        // 这条链路上一个字节都不会过来（见 badge_proto.h 的 BADGE_MSG_HUD）。
        //
        // 缺 open 字段时按「关着」处理：老主机会跳过它，而「关着」是更保守的那个默认
        // （关着时上下键不发上去，屏幕只是少了一句提示）。
        out->kind = BADGE_MSG_HUD;
        out->hud_open = take_bool(&root, "open", false);
        return true;
    }
    if (strcmp(kind, "voice_text") == 0) {
        // 刚才那段语音转成了什么，以及它到哪了。设备只是**显示**它——和这条链路上别的
        // 消息一样，这里不理解"那句话说了什么"。
        char which[BADGE_META_MAX];

        out->kind = BADGE_MSG_VOICE_TEXT;
        out->voice_text[0] = '\0';
        (void)take_string(&root, "text", out->voice_text, sizeof(out->voice_text));
        which[0] = '\0';
        (void)take_string(&root, "state", which, sizeof(which));
        // 认不出的 state 按 sent 处理：那两种里更"靠后"的一种，说「已发送」而不是让用户
        // 以为自己的话还在排队（真在排队时主机下一次会照实说）。
        out->voice_state = (strcmp(which, "queued") == 0) ? BADGE_VOICE_QUEUED : BADGE_VOICE_SENT;
        return true;
    }
    return false; // 不认识的消息类型：丢掉
}

// --- 编码 -------------------------------------------------------------------
//
// 一个手写的追加器：每条消息都由「固定骨架 + 若干要转义的字符串」组成，用 snprintf
// 拼会很难保证「要么完整、要么什么都不写」——snprintf 的返回值是「本可以写多长」，
// 拿它当长度用会写出半个消息，而那在串口上表现为一条永远解析不了的行。

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool ok;
} writer_t;

static void writer_init(writer_t *w, char *out, size_t cap)
{
    w->out = out;
    w->cap = cap;
    w->len = 0;
    w->ok = (out != NULL && cap > 0U);
    if (w->ok) {
        out[0] = '\0';
    }
}

static void writer_raw(writer_t *w, const char *text, size_t length)
{
    if (!w->ok || w->len + length + 1U > w->cap) {
        w->ok = false;
        return;
    }
    memcpy(w->out + w->len, text, length);
    w->len += length;
    w->out[w->len] = '\0';
}

static void writer_literal(writer_t *w, const char *text)
{
    writer_raw(w, text, strlen(text));
}

static void writer_ulong(writer_t *w, unsigned long value)
{
    char buffer[24];
    int written = snprintf(buffer, sizeof(buffer), "%lu", value);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        w->ok = false;
        return;
    }
    writer_raw(w, buffer, (size_t)written);
}

static void writer_escaped(writer_t *w, const char *text)
{
    char buffer[BADGE_QUESTION_MAX + 8U];
    size_t written;

    if (!w->ok) {
        return;
    }
    written = bjson_escape(buffer, sizeof(buffer), text);
    if (written == 0U) {
        w->ok = false;
        return;
    }
    writer_raw(w, buffer, written);
}

static size_t writer_finish(writer_t *w)
{
    if (!w->ok) {
        if (w->out != NULL && w->cap > 0U) {
            w->out[0] = '\0';
        }
        return 0U;
    }
    return w->len;
}

size_t badge_proto_hello(char *out, size_t cap, const char *firmware)
{
    writer_t w;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"hello\",\"proto\":");
    writer_ulong(&w, (unsigned long)BADGE_PROTO_VERSION);
    writer_literal(&w, ",\"fw\":");
    writer_escaped(&w, firmware != NULL ? firmware : "");
    writer_literal(&w, "}");
    return writer_finish(&w);
}

size_t badge_proto_sync(char *out, size_t cap, unsigned long since)
{
    writer_t w;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"sync\",\"since\":");
    writer_ulong(&w, since);
    writer_literal(&w, "}");
    return writer_finish(&w);
}

size_t badge_proto_key(char *out, size_t cap, const char *key)
{
    writer_t w;

    if (key == NULL) {
        return 0U;
    }
    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"key\",\"key\":");
    writer_escaped(&w, key);
    writer_literal(&w, "}");
    return writer_finish(&w);
}

size_t badge_proto_answer_permission(char *out, size_t cap, unsigned long ref,
                                     const char *decision)
{
    writer_t w;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"answer\",\"ref\":");
    writer_ulong(&w, ref);
    writer_literal(&w, ",\"decision\":");
    writer_escaped(&w, decision);
    writer_literal(&w, "}");
    return writer_finish(&w);
}

size_t badge_proto_answer_questions(char *out, size_t cap, unsigned long ref,
                                    const char *const *keys, const char *const *values,
                                    size_t count)
{
    writer_t w;
    size_t i;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"answer\",\"ref\":");
    writer_ulong(&w, ref);
    // answers 的键是**问题全文**，值与 TUI / desktop 的约定一致（多选由调用方拼好）。
    writer_literal(&w, ",\"answers\":{");
    for (i = 0; i < count; ++i) {
        if (keys[i] == NULL || values[i] == NULL) {
            continue;
        }
        if (i > 0U) {
            writer_literal(&w, ",");
        }
        writer_escaped(&w, keys[i]);
        writer_literal(&w, ":");
        writer_escaped(&w, values[i]);
    }
    writer_literal(&w, "},\"annotations\":{}}");
    return writer_finish(&w);
}
