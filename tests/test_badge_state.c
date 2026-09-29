// 状态机的单元测试。这里覆盖的是设备上最让人抓狂的两类故障：按下去没反应，
// 以及答回去的答案对不上题。两者都能在主机上试出来。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "badge_json.h"
#include "badge_state.h"

static badge_state_t state;
static char payload[1024];

static void fill_permission(badge_msg_t *message, unsigned long ref)
{
    memset(message, 0, sizeof(*message));
    message->kind = BADGE_MSG_ASK;
    message->ref = ref;
    message->ask_kind = BADGE_ASK_PERMISSION;
    (void)strcpy(message->session, "s1");
    (void)strcpy(message->title, "Bash 确认");
    (void)strcpy(message->body, "$ rm -rf build/");
    message->option_count = 3U;
    (void)strcpy(message->options[0].label, "允许一次");
    (void)strcpy(message->options[0].value, BADGE_DECISION_ALLOW_ONCE);
    (void)strcpy(message->options[1].label, "本会话允许");
    (void)strcpy(message->options[1].value, BADGE_DECISION_ALLOW_SESSION);
    (void)strcpy(message->options[2].label, "拒绝");
    (void)strcpy(message->options[2].value, BADGE_DECISION_DENY);
}

static void fill_questions(badge_msg_t *message, unsigned long ref, bool multi, size_t count)
{
    size_t i;

    memset(message, 0, sizeof(*message));
    message->kind = BADGE_MSG_ASK;
    message->ref = ref;
    message->ask_kind = BADGE_ASK_QUESTIONS;
    (void)strcpy(message->session, "s2");
    (void)strcpy(message->title, "方向");
    message->question_count = count;
    for (i = 0; i < count; ++i) {
        badge_question_t *question = &message->questions[i];
        char text[32];

        (void)snprintf(text, sizeof(text), "第 %u 题？", (unsigned)(i + 1U));
        (void)strcpy(question->question, text);
        (void)strcpy(question->header, "方向");
        question->multi_select = multi;
        question->option_count = 2U;
        (void)strcpy(question->options[0].label, "甲");
        (void)strcpy(question->options[0].value, "甲");
        (void)strcpy(question->options[1].label, "乙");
        (void)strcpy(message->questions[i].options[1].value, "乙");
    }
}

static void fill_state(badge_msg_t *message, const char *id, const char *label)
{
    memset(message, 0, sizeof(*message));
    message->kind = BADGE_MSG_STATE;
    message->session_count = 1U;
    (void)strcpy(message->sessions[0].id, id);
    (void)strcpy(message->sessions[0].title, "修 lint");
    (void)strcpy(message->sessions[0].label, label);
    (void)strcpy(message->sessions[0].detail, "推理中…");
}

// 断言发出去的是一条权限回答，并返回它的 decision。
static const char *answer_decision(size_t length)
{
    bjson_t json = {.buf = payload, .len = length};
    bjson_val_t root;
    bjson_val_t value;
    static char text[32];

    assert(length > 0U);
    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "t", &value));
    bjson_str_into(&value, text, sizeof(text), NULL);
    assert(strcmp(text, "answer") == 0);
    assert(bjson_obj_get(&root, "decision", &value));
    bjson_str_into(&value, text, sizeof(text), NULL);
    return text;
}

static unsigned long answer_ref(size_t length)
{
    bjson_t json = {.buf = payload, .len = length};
    bjson_val_t root;
    bjson_val_t value;

    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "ref", &value));
    return (unsigned long)bjson_int(&value, -1L);
}

static void test_state_message_moves_the_view(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;

    badge_state_init(&state);
    fill_state(&message, "s1", "执行");
    message.sessions[0].tools = 4UL;
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_STATUS);
    assert(snapshot.has_session);
    assert(strcmp(snapshot.state_label, "执行") == 0);
    assert(strcmp(snapshot.session_title, "修 lint") == 0);
    assert(snapshot.tool_calls == 4UL);

    // 计数往前走（这一轮又跑起来一个工具）必须算作**变了**：去重是按整份会话表
    // memcmp 的，漏掉这个字段的话，屏幕上那行数字会停在上一个读数上——而它存在的
    // 全部意义就是「在动」。
    message.sessions[0].tools = 5UL;
    assert(badge_state_apply(&state, &message));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.tool_calls == 5UL);

    // 同样的内容再来一次不算变化：界面不该为没变的东西重绘。
    assert(!badge_state_apply(&state, &message));
}

static void test_permission_cursor_wraps_and_answers(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_permission(&message, 7UL);
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_ASK);
    assert(snapshot.ask != NULL && snapshot.ask->ref == 7UL);
    assert(snapshot.selection == 0U);

    // 第一个键不该发出任何东西。
    assert(!badge_state_key(&state, BADGE_KEY_UP, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.selection == 2U); // 首项往上绕回末项

    assert(!badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.selection == 0U);

    // 选中「本会话允许」并提交。
    assert(!badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length));
    assert(badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(answer_ref(length) == 7UL);
    assert(strcmp(answer_decision(length), BADGE_DECISION_ALLOW_SESSION) == 0);
}

// 待答项被主机撤销（有人在电脑上答掉了）：它必须从队列里消失，游标回到原点。
static void test_ask_gone_drops_and_resets(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_permission(&message, 1UL);
    assert(badge_state_apply(&state, &message));
    (void)badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length);

    memset(&message, 0, sizeof(message));
    message.kind = BADGE_MSG_ASK_GONE;
    message.ref = 1UL;
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_STATUS);

    // 第二条待答项进来时游标必须是干净的：否则「确定」按下去答的是没看过的那一项。
    fill_permission(&message, 2UL);
    assert(badge_state_apply(&state, &message));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.selection == 0U);
}

// 同一条待答项被主机重发（题面在流式阶段被改写）：原地更新，屏幕上不多出一条。
static void test_repeated_ask_updates_in_place(void)
{
    badge_msg_t message;

    badge_state_init(&state);
    fill_permission(&message, 5UL);
    assert(badge_state_apply(&state, &message));
    (void)strcpy(message.body, "$ rm -rf dist/");
    assert(badge_state_apply(&state, &message));
    assert(state.ask_count == 1U);
    assert(strcmp(state.asks[0].body, "$ rm -rf dist/") == 0);
}

static void test_multi_select_requires_an_explicit_submit(void)
{
    badge_msg_t message;
    size_t length = 0;

    badge_state_init(&state);
    fill_questions(&message, 3UL, true, 1U);
    assert(badge_state_apply(&state, &message));

    // 勾第一项：不发出任何东西。
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(state.picked[0] == 1U);

    // 下行再勾：两项都记着。
    (void)badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length);
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(state.picked[0] == 3U);

    // 取消第一项。
    assert(!badge_state_key(&state, BADGE_KEY_UP, payload, sizeof(payload), &length));
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(state.picked[0] == 2U);

    // 长按提交。
    assert(badge_state_key(&state, BADGE_KEY_SUBMIT, payload, sizeof(payload), &length));
    assert(answer_ref(length) == 3UL);

    // 只勾了「乙」，答案里就该只有它。
    assert(strstr(payload, "乙") != NULL);
    assert(strstr(payload, "甲") == NULL);
}

static void test_multiple_questions_advance_then_submit(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_questions(&message, 4UL, false, 2U);
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 0U);

    // 第一题选第二项：进入第二题，不提交。
    (void)badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length);
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 1U);
    assert(snapshot.selection == 0U); // 新题从第一项开始

    // 第二题选第一项：两题都答完了才提交。
    assert(badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(answer_ref(length) == 4UL);
    assert(strstr(payload, "第 1 题？") != NULL);
    assert(strstr(payload, "第 2 题？") != NULL);
    assert(strstr(payload, "乙") != NULL); // 第一题选的是第二项
}

// 单选题答错一题不该整条重来：长按上键退回上一题。
static void test_single_select_can_go_back(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_questions(&message, 6UL, false, 2U);
    assert(badge_state_apply(&state, &message));

    // 第一题按确定会自动进入第二题（单选没有「跳过去不答」的中间态）。
    (void)badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length);
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 1U);

    (void)badge_state_key(&state, BADGE_KEY_PREV, payload, sizeof(payload), &length);
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 0U);
    assert(snapshot.selection == 0U); // 回到的题也从第一项开始
}

// 多选 + 多题：短按确定被「勾选」占着、长按确定是提交，所以换题只能由上/下键的长按
// 承担。少了这条通道，第一题是多选时整条 ask 都答不完——屏幕只会反复说
// 「还有问题没有作答」，而没有任何键能走到第二题。
static void test_multi_select_walks_between_questions(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;
    const char *first;
    const char *second;

    badge_state_init(&state);
    fill_questions(&message, 9UL, true, 2U); // 两题都是多选
    assert(badge_state_apply(&state, &message));

    // 第一题：勾第一项，长按下键去第二题。
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(!badge_state_key(&state, BADGE_KEY_NEXT, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 1U);
    assert(snapshot.checked == 0U); // 新题从干净的勾选开始

    // 第二题：勾第二项，长按提交。两题都答了，所以这次必须发出去。
    (void)badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length);
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(badge_state_key(&state, BADGE_KEY_SUBMIT, payload, sizeof(payload), &length));
    assert(answer_ref(length) == 9UL);

    // 每题各自的答案都在，并且按题号顺序配对（第一题勾的是甲，第二题是乙）。
    first = strstr(payload, "甲");
    second = strstr(payload, "乙");
    assert(first != NULL && second != NULL && first < second);
}

// 两端的边界：第一题上「上一题」、末题上「下一题」都不动，也都不会顺手提交。
static void test_question_navigation_stops_at_the_ends(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_questions(&message, 10UL, false, 2U);
    assert(badge_state_apply(&state, &message));

    assert(!badge_state_key(&state, BADGE_KEY_PREV, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 0U);

    assert(!badge_state_key(&state, BADGE_KEY_NEXT, payload, sizeof(payload), &length));
    assert(!badge_state_key(&state, BADGE_KEY_NEXT, payload, sizeof(payload), &length));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.question_index == 1U);

    // 跳过去的那题没作答：提交要被拒，而不是发出一个残缺的 answers。
    assert(!badge_state_key(&state, BADGE_KEY_SUBMIT, payload, sizeof(payload), &length));
    assert(strcmp(state.notice, "还有问题没有作答") == 0);
}

// 主机换了一条连接（ref 从零重新分配）时会先发 reset：设备手里那些再也等不到答案的
// 等待项必须全部丢掉。不清的话，同一条等待会以新 ref 再入队一次——屏幕上两条，而旧的
// 那条按下去没有任何反应。
static void test_reset_drops_every_ask(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t length = 0;

    badge_state_init(&state);
    fill_permission(&message, 7UL);
    assert(badge_state_apply(&state, &message));
    fill_questions(&message, 8UL, true, 2U);
    assert(badge_state_apply(&state, &message));
    assert(state.ask_count == 2U);

    // 把游标挪开，验证 reset 也把它归零：新连接的队列该从第一题第一项开始。
    (void)badge_state_key(&state, BADGE_KEY_DOWN, payload, sizeof(payload), &length);
    assert(state.selection == 1U);

    memset(&message, 0, sizeof(message));
    message.kind = BADGE_MSG_RESET;
    assert(badge_state_apply(&state, &message));
    assert(state.ask_count == 0U);
    assert(state.selection == 0U);
    assert(state.question_index == 0U);

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_STATUS); // 屏幕上回到状态屏

    // 队列本来就是空的：再来一条 reset 不算变化，界面不该为它重绘。
    assert(!badge_state_apply(&state, &message));

    // 主机随后把同一条等待重新推下来：它作为新的一条进队，游标是干净的。
    fill_permission(&message, 7UL);
    assert(badge_state_apply(&state, &message));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_ASK);
    assert(snapshot.selection == 0U);
}

// 有些题上不了屏时必须说出来：用户答完眼前这几题就提交，而模型会收到一个少了几条的
// 答复，谁都不知道少了什么。这条提示用的是「一次性提示」那套（几秒后自己消失）。
static void test_dropped_questions_are_disclosed(void)
{
    static const char overflow[] =
        "{\"t\":\"ask\",\"seq\":7,\"ref\":7,\"kind\":\"ask_user\",\"title\":\"方向\","
        "\"session\":\"s1\",\"questions\":["
        "{\"header\":\"一\",\"question\":\"第一题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"二\",\"question\":\"第二题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"三\",\"question\":\"第三题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"四\",\"question\":\"第四题？\",\"options\":[{\"label\":\"A\"}]},"
        "{\"header\":\"五\",\"question\":\"第五题？\",\"options\":[{\"label\":\"A\"}]}]}";
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;

    badge_state_init(&state);
    assert(badge_proto_parse(overflow, strlen(overflow), &message));
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.notice != NULL);
    assert(strcmp(snapshot.notice, "还有 1 题没上屏，到电脑上答") == 0);

    // 放得下的不该有这条提示：它是「你看的不全」的信号，不是装饰。
    badge_state_init(&state);
    fill_questions(&message, 8UL, false, 3U);
    assert(badge_state_apply(&state, &message));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.notice == NULL);
}

// 会话总数要原样带给界面：底栏按主机报的数说「另有 N 个会话在跑」，而不是按屏幕上
// 放得下的那几个——放不下的同样是「另有」。
static void test_session_total_reaches_the_ui(void)
{
    static const char line[] =
        "{\"t\":\"state\",\"seq\":5,\"sessions\":["
        "{\"id\":\"s1\",\"title\":\"一\",\"label\":\"执行\"},"
        "{\"id\":\"s2\",\"title\":\"二\"},{\"id\":\"s3\",\"title\":\"三\"},"
        "{\"id\":\"s4\",\"title\":\"四\"},{\"id\":\"s5\",\"title\":\"五\"},"
        "{\"id\":\"s6\",\"title\":\"六\"}]}";
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;

    badge_state_init(&state);
    assert(badge_proto_parse(line, strlen(line), &message));
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.session_count == BADGE_MAX_SESSIONS); // 放得下的
    assert(snapshot.session_total == 6U);                 // 主机报的
}

// 答案缓冲够不够，决定的不是「好看」而是**按下去有没有反应**：答案的键是题面全文，
// 4 道稍长的题就能超过旧的 512 字节缓冲，而那时设备会静默地什么都不发。
static void build_long_questions(badge_msg_t *message, size_t count, size_t text_len)
{
    size_t i;
    size_t j;

    memset(message, 0, sizeof(*message));
    message->kind = BADGE_MSG_ASK;
    message->ref = 9UL;
    message->ask_kind = BADGE_ASK_QUESTIONS;
    (void)strcpy(message->session, "s1");
    (void)strcpy(message->title, "方向");
    message->question_count = count;
    message->questions_total = count;
    for (i = 0; i < count; ++i) {
        badge_question_t *question = &message->questions[i];

        for (j = 0; j < text_len && j + 1U < sizeof(question->question); ++j) {
            question->question[j] = 'q';
        }
        question->question[j] = '\0';
        question->option_count = 1U;
        (void)strcpy(question->options[0].label, "A");
        (void)strcpy(question->options[0].value, "A");
    }
}

static void test_a_long_four_question_answer_needs_the_bigger_buffer(void)
{
    badge_msg_t message;
    char answer[BADGE_ANSWER_MAX];
    size_t length = 0;
    size_t i;

    build_long_questions(&message, BADGE_MAX_QUESTIONS, 300U);
    badge_state_init(&state);
    assert(badge_state_apply(&state, &message));

    // 单选：每一题的确定都是「选中并前进」，最后一题才提交。
    for (i = 0; i < BADGE_MAX_QUESTIONS; ++i) {
        const bool sent = badge_state_key(&state, BADGE_KEY_OK, answer, sizeof(answer), &length);

        assert(sent == (i + 1U == BADGE_MAX_QUESTIONS));
    }
    assert(length > 512U); // 旧的 512 字节缓冲装不下——那正是它静默失败的那个场景
    assert(strstr(answer, "\"t\":\"answer\"") != NULL);
    assert(state.notice[0] == '\0'); // 编码成功就不该有那条提示
}

// 装不下时必须说一句：屏幕上「按了没反应」是这块板上最贵的一类故障，它连日志都没有。
static void test_unencodable_answer_is_disclosed(void)
{
    badge_msg_t message;
    char tiny[64];
    size_t length = 0;

    fill_questions(&message, 11UL, false, 2U);
    badge_state_init(&state);
    assert(badge_state_apply(&state, &message));

    (void)badge_state_key(&state, BADGE_KEY_OK, tiny, sizeof(tiny), &length); // 第一题：只前进
    assert(!badge_state_key(&state, BADGE_KEY_OK, tiny, sizeof(tiny), &length));
    assert(strcmp(state.notice, "答案装不下，到电脑上答") == 0);
}

// 一道题都显示不了（题面都太长）时，待答屏**仍然要立起来**：界面会写「这题要到电脑上答」。
// 主机正等一个答案，而「有人在等你」必须看得见——看不见的等待等于没有等待。
static void test_an_undisplayable_ask_still_raises_the_screen(void)
{
    char filler[BADGE_QUESTION_MAX + 32U];
    char line[sizeof(filler) + 256U];
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;
    size_t i;

    for (i = 0; i < sizeof(filler) - 1U; ++i) {
        filler[i] = 'a';
    }
    filler[sizeof(filler) - 1U] = '\0';
    (void)snprintf(line, sizeof(line),
                   "{\"t\":\"ask\",\"seq\":9,\"ref\":9,\"kind\":\"ask_user\","
                   "\"session\":\"s1\",\"title\":\"题面太长\","
                   "\"questions\":[{\"question\":\"%s\",\"options\":[{\"label\":\"A\"}]}]}",
                   filler);

    badge_state_init(&state);
    assert(badge_proto_parse(line, strlen(line), &message));
    assert(badge_state_apply(&state, &message));
    assert(state.ask_count == 1U);

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.view == BADGE_UI_ASK);
    assert(snapshot.ask != NULL);
    assert(snapshot.ask->question_count == 0U); // 一道都显示不了
}

// 主机拒绝了回答：屏幕上要看得见，否则按下去没反应像是设备坏了。
static void test_error_sets_a_visible_notice(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;

    badge_state_init(&state);
    memset(&message, 0, sizeof(message));
    message.kind = BADGE_MSG_ERROR;
    message.ref = 9UL;
    (void)strcpy(message.message, "no such pending request");
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.notice != NULL);
    assert(strcmp(snapshot.notice, "no such pending request") == 0);

    // 提示会自己消失。
    assert(!badge_state_tick(&state, 1000U));
    assert(badge_state_tick(&state, 6000U));
    badge_state_to_ui(&state, &snapshot);
    assert(snapshot.notice == NULL);
}

// 队列满了以后，后来的待答项被丢掉——但不能把已经排队的弄乱。
static void test_queue_is_bounded(void)
{
    badge_msg_t message;
    size_t i;

    badge_state_init(&state);
    for (i = 0; i < BADGE_MAX_ASKS; ++i) {
        fill_permission(&message, (unsigned long)(i + 1U));
        assert(badge_state_apply(&state, &message));
    }
    assert(state.ask_count == BADGE_MAX_ASKS);

    fill_permission(&message, 99UL);
    assert(!badge_state_apply(&state, &message));
    assert(state.ask_count == BADGE_MAX_ASKS);
    assert(state.asks[0].ref == 1UL); // 队首没被动过
}

// 没有待答项时，三个键都没有意义——状态屏不是一个可以按的地方。
static void test_keys_do_nothing_without_an_ask(void)
{
    size_t length = 123U;

    badge_state_init(&state);
    assert(!badge_state_key(&state, BADGE_KEY_OK, payload, sizeof(payload), &length));
    assert(length == 0U);
    assert(!badge_state_key(&state, BADGE_KEY_UP, payload, sizeof(payload), &length));
}

// 状态屏优先显示「正在等你的那个会话」，而不是列表里的第一个。
static void test_ui_prefers_the_session_that_is_waiting(void)
{
    badge_msg_t message;
    badge_ui_snapshot_t snapshot;

    badge_state_init(&state);
    memset(&message, 0, sizeof(message));
    message.kind = BADGE_MSG_STATE;
    message.session_count = 2U;
    (void)strcpy(message.sessions[0].id, "idle-one");
    (void)strcpy(message.sessions[0].label, "思考");
    (void)strcpy(message.sessions[1].id, "waiting-one");
    (void)strcpy(message.sessions[1].label, "提问");
    (void)strcpy(message.sessions[1].title, "在等我的那个");
    assert(badge_state_apply(&state, &message));

    fill_permission(&message, 8UL);
    (void)strcpy(message.session, "waiting-one");
    assert(badge_state_apply(&state, &message));

    badge_state_to_ui(&state, &snapshot);
    assert(strcmp(snapshot.session_title, "在等我的那个") == 0);
    assert(strcmp(snapshot.state_label, "提问") == 0);
}

// 提示音的两种含义各记一份，而且**以最后那条为准**：连着两声的意义是零，而漏掉最后那条
// 才是真会让人错过东西。
//
// 一条 ask 本身不再触发声音——那件事现在由 alert 消息说（见 test_alert_message）。
static void test_alert_kinds_reach_the_state(void)
{
    badge_msg_t message;

    badge_state_init(&state);
    fill_permission(&message, 1UL);
    assert(badge_state_apply(&state, &message));
    assert(state.alert_pending == BADGE_ALERT_NONE);

    memset(&message, 0, sizeof(message));
    message.kind = BADGE_MSG_ALERT;
    message.alert_kind = BADGE_ALERT_ASK;
    assert(badge_state_apply(&state, &message));
    assert(state.alert_pending == BADGE_ALERT_ASK);

    message.alert_kind = BADGE_ALERT_DONE; // 还没放掉就又来一条：盖掉
    assert(badge_state_apply(&state, &message));
    assert(state.alert_pending == BADGE_ALERT_DONE);

    state.alert_pending = BADGE_ALERT_NONE; // 调用方消费掉（main 的循环就是这么做的）
    assert(badge_state_apply(&state, &message));
    assert(state.alert_pending == BADGE_ALERT_DONE);
}

int main(void)
{
    test_state_message_moves_the_view();
    test_permission_cursor_wraps_and_answers();
    test_ask_gone_drops_and_resets();
    test_repeated_ask_updates_in_place();
    test_multi_select_requires_an_explicit_submit();
    test_multiple_questions_advance_then_submit();
    test_single_select_can_go_back();
    test_multi_select_walks_between_questions();
    test_question_navigation_stops_at_the_ends();
    test_reset_drops_every_ask();
    test_dropped_questions_are_disclosed();
    test_session_total_reaches_the_ui();
    test_a_long_four_question_answer_needs_the_bigger_buffer();
    test_unencodable_answer_is_disclosed();
    test_an_undisplayable_ask_still_raises_the_screen();
    test_error_sets_a_visible_notice();
    test_queue_is_bounded();
    test_keys_do_nothing_without_an_ask();
    test_ui_prefers_the_session_that_is_waiting();
    test_alert_kinds_reach_the_state();
    printf("test_badge_state: OK\n");
    return 0;
}
