// badge_proto —— 设备与 tachi 之间的那一层协议。
//
// 一条消息是一行 JSON，每行以 `@@` 开头（`@@` 由传输层负责，见 badge_link）。前缀
// 不是洁癖：设备与控制台日志共用同一条 USB-Serial-JTAG，日志字节一定会混进来，前缀
// 是唯一能把两者分开的东西。tachi 侧的同一份协议在 desktop/link/link.go。
//
// 这一层是纯逻辑：不碰 ESP-IDF、不碰 LVGL、不分配内存，所以它能在主机上被完整测试
// （tests/test_badge_proto.c）。真机上最难查的正是这一层——中文转义、长字段截断、
// 一条消息里数组的长度——而它们在主机上都能试出来。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// 字段上限。都是**字节**上限而不是字符数：截断由 bjson_str_into 落在 UTF-8 边界上，
// 所以按字节定容不会切出半个字。
#define BADGE_META_MAX 24      // status / label / kind / option value 这类机器值
// 64 字节 = 21 个汉字。48 时连「先做设备（需要板子）」这种选项都装不下，
// 而选项文字被截断是「屏幕上只看到一部分」最常见的来源。
#define BADGE_TITLE_MAX 64     // 会话标题、问题 header、按钮文字
#define BADGE_TEXT_MAX 64      // 会话 id
#define BADGE_DETAIL_MAX 96    // 状态副标题、主机报错文本
#define BADGE_BODY_MAX 256     // 权限等待的预览（命令 + 命中的规则，多行）
// 问题全文是回传答案的键，必须完整——所以它宁可让整道题不上屏（见 take_questions），
// 也不截断。384 字节 = 128 个汉字，超出这个长度的题本来就该在电脑上回答。
#define BADGE_QUESTION_MAX 384
#define BADGE_FIRMWARE_MAX 16

// 一张屏上能放下的量。超出的部分**不再静默丢掉**（见 main/badge_proto.h 的
// *_total 字段）：屏幕上会说一句「还有 N 项在电脑上」，因为看不见的选项等于不存在，
// 而用户无从知道少的是什么。
//
// 选项上限是 5 而不是更多：240x320 的屏上，正文框和右上角电量占掉之后，选项区
// 恰好放得下 5 行 22px（行距 24px）。多出来的选项在屏幕上放不下，所以真话要占一行
// ——需要报「还有 N 项」时，只显示 4 项 + 那一行；否则显示 5 项，什么都不用说。
#define BADGE_MAX_SESSIONS 4
#define BADGE_MAX_OPTIONS 5
// 4 题是工具 schema 对模型宣称的上限（agent/tools/askuser.go 的 "1-4 questions"），
// 而它只是一句描述、没有强制——所以设备这边对齐到 4，多出来的仍然由那句提示兜住。
// 内存不是理由：一道题 1104 字节，整个 badge_msg_t 5576 字节，加一题多约 8KB 静态区。
#define BADGE_MAX_QUESTIONS 4

// 回传答案的缓冲上限。答案的**键是题面全文**（协议约定），所以最坏情况是
// 每道题的题面 + 它的选项标签：4 道题 × (384 + 64) 再加上 JSON 骨架与转义余量。
// 这个数不是「够用就行」——装不下的后果是按下确定之后什么都不发（见
// badge_state 的 submit_questions），所以调用方的缓冲必须按它开。
#define BADGE_ANSWER_MAX (BADGE_MAX_QUESTIONS * (BADGE_QUESTION_MAX + BADGE_TITLE_MAX + 16U) + 64U)

// 权限回答的三个机器值，与 tachi 侧同一套（desktop/link/link.go）。
#define BADGE_DECISION_ALLOW_ONCE "allow_once"
#define BADGE_DECISION_ALLOW_SESSION "allow_session"
#define BADGE_DECISION_DENY "deny"

typedef enum {
    BADGE_MSG_NONE = 0,
    BADGE_MSG_STATE,    // 会话状态快照
    BADGE_MSG_ASK,      // 一条待答项（权限确认或提问）
    BADGE_MSG_ASK_GONE, // 某条待答项已被撤销
    BADGE_MSG_ERROR,    // 主机侧拒绝了我们的回答
    BADGE_MSG_RESET,    // 主机换了新的 ref 空间（每条连接都会重新分配）
    BADGE_MSG_ALERT,    // 响一声：有事等你，或者一个回合跑完了
} badge_msg_kind_t;

// 提示音的两种含义。设备为它们放**不同的音频**——「有事找你」和「好了」是两件事，
// 人不用看屏幕就能从声音上分开（见 badge_sound）。
typedef enum {
    BADGE_ALERT_NONE = 0, // 没有（还是被处理掉了）
    BADGE_ALERT_ASK,      // 出现了一条等你回答的等待（权限确认 / 提问）
    BADGE_ALERT_DONE,     // 一个回合跑完了
} badge_alert_kind_t;

typedef enum {
    BADGE_ASK_PERMISSION = 0, // 一条命令要放行：选项是允许/拒绝
    BADGE_ASK_QUESTIONS,      // 模型提问：选项来自模型
} badge_ask_kind_t;

typedef struct {
    char id[BADGE_TEXT_MAX];
    char title[BADGE_TEXT_MAX];
    char status[BADGE_META_MAX];
    char label[BADGE_META_MAX];
    char detail[BADGE_DETAIL_MAX];
} badge_session_t;

typedef struct {
    char label[BADGE_TITLE_MAX]; // 屏幕上显示的那一项
    // value 是回传的机器值。权限确认的三项由 tachi 给出（allow_once 等），
    // 而提问的选项只有文字——那时答案就是选项文字本身，解析时用它填满这里。
    char value[BADGE_TITLE_MAX];
} badge_option_t;

typedef struct {
    char question[BADGE_QUESTION_MAX]; // 回传答案的键：截断了就答不上
    char header[BADGE_TITLE_MAX];
    bool multi_select;
    size_t option_count;
    badge_option_t options[BADGE_MAX_OPTIONS];
    // options_total 是主机**发来的**项数，option_count 是屏幕上放得下的。
    // 两者不等就要在屏幕上说出来（「还有 N 项在电脑上」）——看不见的选项等于不存在，
    // 而用户无从知道少的是什么。
    size_t options_total;
} badge_question_t;

// badge_msg_t 是一条解析好的消息。它把一行里出现过的字段都摊平在一个结构里
// （而不是 union）：字段本身很小，省下来的那点内存不值得让每次访问都要先判类型。
//
// 它有几个 KB，调用方应当放在静态存储上——任务栈（几 KB）放不下它。
typedef struct {
    badge_msg_kind_t kind;
    unsigned long seq;

    // BADGE_MSG_STATE
    size_t session_count;
    badge_session_t sessions[BADGE_MAX_SESSIONS];
    size_t sessions_total; // 主机报了多少（见 badge_question_t.options_total）

    // BADGE_MSG_ASK
    unsigned long ref; // 本次连接内的行号，回答时原样带回
    char session[BADGE_TEXT_MAX]; // 哪个会话在等：状态屏据此把那一行挑出来显示
    badge_ask_kind_t ask_kind;
    char title[BADGE_TITLE_MAX];
    char body[BADGE_BODY_MAX]; // 权限：预览文本；提问：不用
    size_t option_count;
    badge_option_t options[BADGE_MAX_OPTIONS];
    size_t options_total;
    size_t question_count;
    badge_question_t questions[BADGE_MAX_QUESTIONS];
    // 主机发来的题数。比 question_count 多，就说明有些题上不了屏（超出上限，或者
    // 题面长到装不下——题面是回传答案的键，截断了就答不上，所以那种题整道不显示）。
    size_t questions_total;

    // BADGE_MSG_ERROR
    char message[BADGE_DETAIL_MAX];

    // BADGE_MSG_ALERT：放哪一段音频（BADGE_ALERT_NONE 表示这条消息不认识，忽略）
    badge_alert_kind_t alert_kind;
} badge_msg_t;

// badge_proto_parse 解析一行（`@@` 前缀与行尾已由传输层剥掉）。
//
// 返回 true 表示「这是一条我们认识的消息」；false 表示这行该被丢掉——它是坏数据、
// 是未知类型、或者根本不是协议行。两种情况调用方的动作完全一样，所以不区分。
bool badge_proto_parse(const char *line, size_t length, badge_msg_t *out);

// 下面几个把设备要发的消息编进 out，返回写出的字节数（不含结尾 NUL）；cap 不够
// 返回 0，此时 out 里不会有半个消息——调用方据此丢掉这一条而不是发出半条。
//
// 编码后的行**不含** `@@` 前缀与换行：那是传输层的事。
size_t badge_proto_hello(char *out, size_t cap, const char *firmware);
size_t badge_proto_sync(char *out, size_t cap, unsigned long since);
size_t badge_proto_answer_permission(char *out, size_t cap, unsigned long ref,
                                     const char *decision);

// badge_proto_answer_questions 组装提问的回答。keys 是**问题全文**（必须与收到的
// 一字不差，所以调用方直接用 badge_question_t.question），values 是选中的标签，
// 多选时由调用方先用 ", " 拼好——这与 TUI / desktop 的约定一致，模型看到的是同一个形状。
size_t badge_proto_answer_questions(char *out, size_t cap, unsigned long ref,
                                    const char *const *keys, const char *const *values,
                                    size_t count);
