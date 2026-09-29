// 协议层的单元测试。样本是按 tachi 侧真实的输出写的（desktop/link/link.go 用
// Go 的 encoding/json 生成，所以 < > & 会是 \uXXXX 形式），不是凭空构造的。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "badge_json.h"
#include "badge_proto.h"

static badge_msg_t message;

static bool parse(const char *line)
{
    return badge_proto_parse(line, strlen(line), &message);
}

static void test_state(void)
{
    static const char line[] =
        "{\"t\":\"state\",\"seq\":5,\"sessions\":["
        "{\"id\":\"2026-09-28-ab12\",\"title\":\"修 lint\",\"status\":\"tool_running\","
        "\"label\":\"执行\",\"detail\":\"调用 Bash\"},"
        "{\"id\":\"s2\",\"title\":\"第二个\",\"status\":\"thinking\",\"label\":\"思考\"}]}";

    assert(parse(line));
    assert(message.kind == BADGE_MSG_STATE);
    assert(message.seq == 5UL);
    assert(message.session_count == 2U);
    assert(strcmp(message.sessions[0].id, "2026-09-28-ab12") == 0);
    assert(strcmp(message.sessions[0].title, "修 lint") == 0);
    assert(strcmp(message.sessions[0].detail, "调用 Bash") == 0);
    assert(strcmp(message.sessions[1].label, "思考") == 0);
    // 缺 detail 的会话留空，不是垃圾内容。
    assert(message.sessions[1].detail[0] == '\0');
}

// 桌面上什么都没跑：这是一条合法消息，不是坏数据。
static void test_state_without_sessions(void)
{
    assert(parse("{\"t\":\"state\",\"seq\":9,\"sessions\":[]}"));
    assert(message.kind == BADGE_MSG_STATE);
    assert(message.session_count == 0U);
}

static void test_permission_ask(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":6,\"ref\":6,\"kind\":\"permission\",\"session\":\"s1\","
        "\"toolId\":\"call_1\",\"title\":\"Bash 确认\",\"body\":\"$ rm -rf build/ \\u0026\\u0026 "
        "make\",\"options\":["
        "{\"label\":\"允许一次\",\"value\":\"allow_once\"},"
        "{\"label\":\"本会话允许\",\"value\":\"allow_session\"},"
        "{\"label\":\"拒绝\",\"value\":\"deny\"}]}";

    assert(parse(line));
    assert(message.kind == BADGE_MSG_ASK);
    assert(message.ref == 6UL);
    assert(message.ask_kind == BADGE_ASK_PERMISSION);
    assert(strcmp(message.title, "Bash 确认") == 0);
    // 命令里的 & 是 Go 侧的 \u0026，解出来必须是原样的字符。
    assert(strcmp(message.body, "$ rm -rf build/ && make") == 0);
    assert(message.option_count == 3U);
    assert(strcmp(message.options[0].label, "允许一次") == 0);
    assert(strcmp(message.options[0].value, BADGE_DECISION_ALLOW_ONCE) == 0);
    assert(strcmp(message.options[2].value, BADGE_DECISION_DENY) == 0);
    assert(message.question_count == 0U);
}

static void test_question_ask(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":7,\"ref\":7,\"kind\":\"ask_user\",\"session\":\"s1\","
        "\"toolId\":\"call_2\",\"title\":\"方向\",\"questions\":[{"
        "\"header\":\"方向\",\"question\":\"走哪条路？\",\"multi_select\":true,\"options\":["
        "{\"label\":\"先做设备\",\"description\":\"需要板子\"},"
        "{\"label\":\"先做电脑\",\"description\":\"不用板子\"}]}]}";

    assert(parse(line));
    assert(message.kind == BADGE_MSG_ASK);
    assert(message.ask_kind == BADGE_ASK_QUESTIONS);
    assert(message.question_count == 1U);
    assert(strcmp(message.questions[0].question, "走哪条路？") == 0);
    assert(strcmp(message.questions[0].header, "方向") == 0);
    assert(message.questions[0].multi_select);
    assert(message.questions[0].option_count == 2U);
    // 提问的选项值来自模型，不是机器值——设备原样回传即可。
    assert(strcmp(message.questions[0].options[1].label, "先做电脑") == 0);
}

static void test_ask_gone_and_error(void)
{
    assert(parse("{\"t\":\"ask_gone\",\"seq\":8,\"ref\":6}"));
    assert(message.kind == BADGE_MSG_ASK_GONE);
    assert(message.ref == 6UL);

    assert(parse("{\"t\":\"error\",\"seq\":9,\"ref\":6,\"message\":\"no such pending request\"}"));
    assert(message.kind == BADGE_MSG_ERROR);
    assert(message.ref == 6UL);
    assert(strcmp(message.message, "no such pending request") == 0);
}

// 放不下的选项要**留一行说真话**：7 项时只上屏 4 项，总数记成 7——多出来的那一行
// 是「还有 3 项在电脑上」，而看不见的选项等于不存在。
static void test_option_overflow_reserves_a_row(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":6,\"ref\":6,\"kind\":\"permission\",\"title\":\"Bash 确认\","
        "\"options\":["
        "{\"label\":\"一\",\"value\":\"a\"},{\"label\":\"二\",\"value\":\"b\"},"
        "{\"label\":\"三\",\"value\":\"c\"},{\"label\":\"四\",\"value\":\"d\"},"
        "{\"label\":\"五\",\"value\":\"e\"},{\"label\":\"六\",\"value\":\"f\"},"
        "{\"label\":\"七\",\"value\":\"g\"}]}";

    assert(parse(line));
    assert(message.kind == BADGE_MSG_ASK);
    assert(message.options_total == 7U);
    assert(message.option_count == BADGE_MAX_OPTIONS - 1U); // 留一行给那句提示
    assert(strcmp(message.options[message.option_count - 1U].label, "四") == 0);
}

// 正好放得下就不该有那句话：提示行只在真有东西看不见时才占位。
static void test_exact_option_fit_has_no_overflow(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":6,\"ref\":6,\"kind\":\"permission\",\"title\":\"Bash 确认\","
        "\"options\":["
        "{\"label\":\"一\",\"value\":\"a\"},{\"label\":\"二\",\"value\":\"b\"},"
        "{\"label\":\"三\",\"value\":\"c\"},{\"label\":\"四\",\"value\":\"d\"},"
        "{\"label\":\"五\",\"value\":\"e\"}]}";

    assert(parse(line));
    assert(message.options_total == BADGE_MAX_OPTIONS);
    assert(message.option_count == BADGE_MAX_OPTIONS);
}

// 会话也一样：主机报了几个就记几个，底栏才不会少报「另有 N 个会话在跑」。
static void test_session_overflow_is_counted(void)
{
    static const char line[] =
        "{\"t\":\"state\",\"seq\":5,\"sessions\":["
        "{\"id\":\"s1\",\"title\":\"一\"},{\"id\":\"s2\",\"title\":\"二\"},"
        "{\"id\":\"s3\",\"title\":\"三\"},{\"id\":\"s4\",\"title\":\"四\"},"
        "{\"id\":\"s5\",\"title\":\"五\"},{\"id\":\"s6\",\"title\":\"六\"}]}";

    assert(parse(line));
    assert(message.session_count == BADGE_MAX_SESSIONS);
    assert(message.sessions_total == 6U);
}

// 题数超出上限、或者某道题的题面长到装不下，都算「没上屏」——两种对用户是同一件事，
// 而 device 必须能说出少了几个（见 badge_state 的 disclose_dropped_questions）。
static void test_question_overflow_is_counted(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":7,\"ref\":7,\"kind\":\"ask_user\",\"title\":\"方向\","
        "\"questions\":["
        "{\"header\":\"一\",\"question\":\"第一题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"二\",\"question\":\"第二题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"三\",\"question\":\"第三题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"四\",\"question\":\"第四题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"五\",\"question\":\"第五题？\",\"options\":[{\"label\":\"A\"}]}]}";

    assert(parse(line));
    assert(message.questions_total == 5U);
    assert(message.question_count == BADGE_MAX_QUESTIONS);
}

// 主机换了 ref 空间时必须能被解析出来：这条消息没有 ref、没有正文，唯一的作用是
// 让设备丢掉手里的旧等待项。它不该被当成坏消息丢掉。
static void test_reset(void)
{
    assert(parse("{\"t\":\"reset\",\"seq\":12}"));
    assert(message.kind == BADGE_MSG_RESET);
    assert(message.option_count == 0U);
    assert(message.question_count == 0U);
}

// 答不回去的消息不该上屏：一条没有 ref 的 ask 在屏幕上是个按不动的按钮。
static void test_ask_without_ref_is_dropped(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":6,\"kind\":\"permission\",\"title\":\"Bash 确认\","
        "\"options\":[{\"label\":\"允许\",\"value\":\"allow_once\"}]}";
    static const char no_options[] =
        "{\"t\":\"ask\",\"seq\":6,\"ref\":6,\"kind\":\"permission\",\"title\":\"Bash 确认\","
        "\"body\":\"x\"}";

    assert(!parse(line));
    assert(!parse(no_options));
}

// 没有选项的自由输入题也要收下：设备答不了它，但「有人在等」这件事必须让人知道，
// 丢掉它等于把一次等待变成静默。
static void test_question_without_options_is_kept(void)
{
    static const char line[] =
        "{\"t\":\"ask\",\"seq\":1,\"ref\":1,\"kind\":\"ask_user\","
        "\"questions\":[{\"question\":\"你希望这个功能叫什么名字？\","
        "\"header\":\"命名\",\"multi_select\":false,\"options\":[]}]}";

    assert(parse(line));
    assert(message.kind == BADGE_MSG_ASK);
    assert(message.ask_kind == BADGE_ASK_QUESTIONS);
    assert(message.question_count == 1U);
    assert(message.questions[0].option_count == 0U);
    assert(strcmp(message.questions[0].question, "你希望这个功能叫什么名字？") == 0);

    // 字段整个缺失也一样（自由输入的题本来就不会有 options）。
    assert(parse("{\"t\":\"ask\",\"seq\":1,\"ref\":1,\"kind\":\"ask_user\","
                 "\"questions\":[{\"question\":\"然后呢？\"}]}"));
    assert(message.question_count == 1U);
    assert(message.questions[0].option_count == 0U);
}

// 题面是回传答案的键：装不下就丢掉这道题，让用户在电脑上回答。
// 半道题比没有题更坏——模型会收到一个它不认识的答案。
// 题面长到装不下：这道题显示不了（题面是回传答案的键，截断了模型会收到一个它不认识
// 的答案），但**这条 ask 仍然要收下**——主机正等一个答案，而屏幕必须让「有人在等你」
// 看得见。界面据此显示「这题要到电脑上答」。
//
// 这与「没有选项的自由输入题照样收下」是同一条理由：丢掉它等于把一次等待变成静默，
// 而那与这块设备存在的理由正好相反。
static void test_oversized_question_is_kept_but_undisplayable(void)
{
    char line[2048];
    char filler[BADGE_QUESTION_MAX + 64U];
    size_t i;

    for (i = 0; i < sizeof(filler) - 1U; ++i) {
        filler[i] = 'a';
    }
    filler[sizeof(filler) - 1U] = '\0';

    (void)snprintf(line, sizeof(line),
                   "{\"t\":\"ask\",\"seq\":1,\"ref\":1,\"kind\":\"ask_user\",\"title\":\"长题面\","
                   "\"questions\":[{\"question\":\"%s\",\"options\":["
                   "{\"label\":\"A\",\"value\":\"A\"}]}]}",
                   filler);
    assert(parse(line));
    assert(message.kind == BADGE_MSG_ASK);
    assert(message.ask_kind == BADGE_ASK_QUESTIONS);
    assert(message.questions_total == 1U); // 主机问了
    assert(message.question_count == 0U);  // 一道都显示不了
}

// 未知类型、坏 JSON、非协议行：一律丢掉，且不留下半条解析结果。
static void test_unknown_and_garbage(void)
{
    static const char garbage[] = "I (1234) wifi: connected";

    assert(!parse(garbage));
    assert(!parse("{\"t\":\"something_else\",\"seq\":1}"));
    assert(!parse("{"));
    assert(message.kind == BADGE_MSG_NONE);
}

static void test_encoding(void)
{
    char buffer[512];
    size_t length;

    length = badge_proto_hello(buffer, sizeof(buffer), "0.1.0");
    assert(length > 0U);
    assert(strcmp(buffer, "{\"t\":\"hello\",\"proto\":1,\"fw\":\"0.1.0\"}") == 0);

    length = badge_proto_sync(buffer, sizeof(buffer), 41UL);
    assert(length > 0U);
    assert(strcmp(buffer, "{\"t\":\"sync\",\"since\":41}") == 0);

    length = badge_proto_answer_permission(buffer, sizeof(buffer), 6UL,
                                           BADGE_DECISION_ALLOW_SESSION);
    assert(length > 0U);
    assert(strcmp(buffer,
                  "{\"t\":\"answer\",\"ref\":6,\"decision\":\"allow_session\"}") == 0);
}

// 答案的键是问题全文，里面可能有引号和中文——这一趟必须能原样往返，
// 否则模型收到的是一个它认不出来的答案。
static void test_question_answer_round_trip(void)
{
    static const char *const keys[] = {
        "走哪条路？",
        "quote\"and\\backslash",
    };
    static const char *const values[] = {
        "先做设备",
        "A, B",
    };
    char buffer[512];
    bjson_t json;
    bjson_val_t root;
    bjson_val_t answers;
    bjson_val_t entry;
    char decoded[128];
    size_t length;

    length = badge_proto_answer_questions(buffer, sizeof(buffer), 7UL, keys, values, 2U);
    assert(length > 0U);

    json.buf = buffer;
    json.len = length;
    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "t", &entry));
    bjson_str_into(&entry, decoded, sizeof(decoded), NULL);
    assert(strcmp(decoded, "answer") == 0);
    assert(bjson_obj_get(&root, "ref", &entry));
    assert(bjson_int(&entry, -1) == 7);

    assert(bjson_obj_get(&root, "answers", &answers));
    assert(answers.kind == BJSON_OBJECT);
    assert(bjson_obj_get(&answers, keys[0], &entry));
    bjson_str_into(&entry, decoded, sizeof(decoded), NULL);
    assert(strcmp(decoded, values[0]) == 0);
    assert(bjson_obj_get(&answers, keys[1], &entry));
    bjson_str_into(&entry, decoded, sizeof(decoded), NULL);
    assert(strcmp(decoded, values[1]) == 0);
}

// 缓冲不够时返回 0，并且 out 里不留半条消息——半条消息在串口上是一行永远
// 解析不了的东西，比不发更坏。
static void test_encoding_refuses_a_partial_message(void)
{
    char buffer[16];
    char keys[1][BADGE_QUESTION_MAX];
    const char *key_pointers[1];
    const char *value_pointers[1];

    memset(buffer, 'x', sizeof(buffer));
    assert(badge_proto_hello(buffer, sizeof(buffer), "a-very-long-firmware-name") == 0U);
    assert(buffer[0] == '\0');

    memset(buffer, 'x', sizeof(buffer));
    assert(badge_proto_answer_permission(buffer, sizeof(buffer), 123456UL,
                                         BADGE_DECISION_ALLOW_ONCE) == 0U);
    assert(buffer[0] == '\0');

    // 太长的键在编码阶段就被拒绝：bjson_escape 的缓冲是问题字段上限加余量。
    memset(keys[0], 'k', sizeof(keys[0]) - 1U);
    keys[0][sizeof(keys[0]) - 1U] = '\0';
    key_pointers[0] = keys[0];
    value_pointers[0] = "v";
    assert(badge_proto_answer_questions(buffer, sizeof(buffer), 1UL, key_pointers,
                                        value_pointers, 1U) == 0U);
    assert(buffer[0] == '\0');
}

// 提示音是一条**独立的**消息，而不是挂在 ask 上的字段：「有事等你」与「回合完成」是两件
// 事，而后者根本不属于任何一条 ask——挂在 ask 上就表达不了它。
// 语音回执：设备只是显示它，但字段必须解析对——「排队中」与「已发送」是两句话，而那是
// 用户判断「刚才那句话到哪了」的唯一依据。
static void test_voice_text(void)
{
    char long_text[256];

    assert(parse("{\"t\":\"voice_text\",\"seq\":54,\"id\":7,\"text\":\"帮我把这个改动提交一下\","
                 "\"state\":\"queued\"}"));
    assert(message.kind == BADGE_MSG_VOICE_TEXT);
    assert(strcmp(message.voice_text, "帮我把这个改动提交一下") == 0);
    assert(message.voice_state == BADGE_VOICE_QUEUED);

    // 认不出的 state 按「已发送」处理：两种里更靠后的一种，而不是让用户以为话还压在队列里。
    assert(parse("{\"t\":\"voice_text\",\"id\":7,\"text\":\"hi\",\"state\":\"whatever\"}"));
    assert(message.voice_state == BADGE_VOICE_SENT);

    // 文本为空也是合法的一条（主机没认出字）：显示成「已发送：」——不崩、不留上一句的残渣。
    assert(parse("{\"t\":\"voice_text\",\"id\":7,\"text\":\"\",\"state\":\"sent\"}"));
    assert(message.voice_text[0] == '\0');

    // 超长文本被**截断**而不是溢出：屏幕上那一行本来就显示不下，但越界写会踩坏别的字段。
    memset(long_text, 0, sizeof(long_text));
    {
        size_t i;

        for (i = 0; i + 3U < 200U; i += 3U) {
            memcpy(&long_text[i], "汉", 3U); // 每个汉字 3 字节
        }
    }
    {
        char line[384];

        (void)snprintf(line, sizeof(line), "{\"t\":\"voice_text\",\"id\":7,\"text\":\"%s\",\"state\":\"sent\"}",
                       long_text);
        assert(parse(line));
    }
    assert(strlen(message.voice_text) < BADGE_VOICE_TEXT_MAX);
}

// 会话信息屏那几行：全是主机排好版的字符串，设备只负责画。空串表示「这一项没有」，
// 所以解析不能把空字段当成坏数据。
static void test_info(void)
{
    assert(parse("{\"t\":\"info\",\"seq\":42,\"title\":\"修 lint\",\"model\":\"gpt-5\","
                 "\"context\":\"12.3k / 32k (39%)\",\"cost\":\"$0.42\"}"));
    assert(message.kind == BADGE_MSG_INFO);
    assert(strcmp(message.info.title, "修 lint") == 0);
    assert(strcmp(message.info.model, "gpt-5") == 0);
    assert(strcmp(message.info.context, "12.3k / 32k (39%)") == 0);
    assert(strcmp(message.info.cost, "$0.42") == 0);

    // 没有活跃会话时主机只给一个 title，其余为空——那几行不画，而不是拿上一屏的残渣。
    assert(parse("{\"t\":\"info\",\"title\":\"（没有活跃会话）\"}"));
    assert(strcmp(message.info.title, "（没有活跃会话）") == 0);
    assert(message.info.model[0] == '\0');
    assert(message.info.cost[0] == '\0');
}

// 设备问的那一条：一小行，没有参数。
static void test_info_request(void)
{
    char line[64];

    assert(badge_proto_info_request(line, sizeof(line)) == strlen("{\"t\":\"info\"}"));
    assert(strcmp(line, "{\"t\":\"info\"}") == 0);
    // 装不下就整条作废（不留半条）。
    assert(badge_proto_info_request(line, 4U) == 0U);
}

static void test_alert_message(void)
{
    assert(parse("{\"t\":\"alert\",\"seq\":9,\"kind\":\"ask\"}"));
    assert(message.kind == BADGE_MSG_ALERT);
    assert(message.alert_kind == BADGE_ALERT_ASK);

    assert(parse("{\"t\":\"alert\",\"seq\":10,\"kind\":\"done\"}"));
    assert(message.kind == BADGE_MSG_ALERT);
    assert(message.alert_kind == BADGE_ALERT_DONE);

    // 不认识的 kind / 没有 kind：丢掉。放错一段声音比不响更让人困惑。
    assert(!parse("{\"t\":\"alert\",\"seq\":11,\"kind\":\"whatever\"}"));
    assert(!parse("{\"t\":\"alert\",\"seq\":12}"));
}

int main(void)
{
    test_state();
    test_state_without_sessions();
    test_permission_ask();
    test_question_ask();
    test_ask_gone_and_error();
    test_option_overflow_reserves_a_row();
    test_exact_option_fit_has_no_overflow();
    test_session_overflow_is_counted();
    test_question_overflow_is_counted();
    test_reset();
    test_ask_without_ref_is_dropped();
    test_question_without_options_is_kept();
    test_oversized_question_is_kept_but_undisplayable();
    test_unknown_and_garbage();
    test_encoding();
    test_question_answer_round_trip();
    test_encoding_refuses_a_partial_message();
    test_voice_text();
    test_info();
    test_info_request();
    test_alert_message();
    printf("test_badge_proto: OK\n");
    return 0;
}
