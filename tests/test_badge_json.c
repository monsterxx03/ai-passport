// 协议解析器的单元测试。这些用例覆盖的是「真机上最容易出错、又最难查」的那一层：
// 转义、容器边界、坏数据、以及按字节截断中文。它们在主机上跑，不需要板子。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "badge_json.h"

// 取值助手：把一个字符串字段解到栈上再看。
static const char *str_of(const bjson_val_t *v, char *buffer, size_t cap)
{
    bjson_str_into(v, buffer, cap, NULL);
    return buffer;
}

static void test_object_fields(void)
{
    static const char line[] = "{\"t\":\"state\",\"seq\":42,\"ok\":true}";
    bjson_t json = {.buf = line, .len = sizeof(line) - 1U};
    bjson_val_t root;
    bjson_val_t value;
    char text[32];

    assert(bjson_parse(&json, &root));
    assert(root.kind == BJSON_OBJECT);

    assert(bjson_obj_get(&root, "t", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "state") == 0);

    assert(bjson_obj_get(&root, "seq", &value));
    assert(bjson_int(&value, -1) == 42);

    assert(bjson_obj_get(&root, "ok", &value));
    assert(bjson_bool(&value, false));

    // 缺字段不是错误：协议里少一个可选字段只是少一条信息。
    assert(!bjson_obj_get(&root, "missing", &value));
    assert(bjson_int(&value, -7) == -7);
    assert(bjson_str_into(&value, text, sizeof(text), NULL) == 0U);
    assert(text[0] == '\0');

    // 同一个对象可以反复查：解析只读，不破坏输入。
    assert(bjson_obj_get(&root, "t", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "state") == 0);
    assert(bjson_obj_get(&root, "seq", &value));
    assert(bjson_int(&value, -1) == 42);
}

// tachi 一侧用 Go 的 encoding/json 生成这些行，它会把 < > & 写成 \uXXXX。
// 中文本身是原样输出的 UTF-8。
static void test_escapes(void)
{
    static const char line[] = "{\"a\":\"\\u003cbash\\u003e\",\"b\":\"\\u4e2d\\u6587\","
                               "\"c\":\"quote\\\" and back\\\\slash\",\"d\":\"tab\\there\"}";
    bjson_t json = {.buf = line, .len = sizeof(line) - 1U};
    bjson_val_t root;
    bjson_val_t value;
    char text[64];

    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "a", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "<bash>") == 0);
    assert(bjson_obj_get(&root, "b", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "中文") == 0);
    assert(bjson_obj_get(&root, "c", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "quote\" and back\\slash") == 0);
    assert(bjson_obj_get(&root, "d", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "tab\there") == 0);

    // 解完之后原文还在，所以再解一次仍然是同样的结果。
    assert(bjson_obj_get(&root, "b", &value));
    assert(strcmp(str_of(&value, text, sizeof(text)), "中文") == 0);
}

// 容器定界不能被字符串里的括号骗到——模型生成的命令里出现 `}` 是常事。
static void test_braces_inside_strings(void)
{
    static const char line[] = "{\"sessions\":[{\"id\":\"a\"},{\"id\":\"}]\",\"detail\":\"x\"}],"
                               "\"after\":1}";
    bjson_t json = {.buf = line, .len = sizeof(line) - 1U};
    bjson_val_t root;
    bjson_val_t sessions;
    bjson_val_t item;
    bjson_val_t id;
    bjson_val_t after;
    char text[32];

    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "sessions", &sessions));
    assert(bjson_arr_len(&sessions) == 2);
    assert(bjson_arr_get(&sessions, 0, &item));
    assert(bjson_obj_get(&item, "id", &id));
    assert(strcmp(str_of(&id, text, sizeof(text)), "a") == 0);
    assert(bjson_arr_get(&sessions, 1, &item));
    assert(bjson_obj_get(&item, "id", &id));
    assert(strcmp(str_of(&id, text, sizeof(text)), "}]") == 0);

    // 数组之后的兄弟键仍然能取到：容器的结束位置算对了。
    assert(bjson_obj_get(&root, "after", &after));
    assert(bjson_int(&after, -1) == 1);
}

static void test_arrays_of_objects(void)
{
    static const char line[] = "{\"options\":[{\"label\":\"允许一次\",\"value\":\"allow_once\"},"
                               "{\"label\":\"拒绝\",\"value\":\"deny\"}]}";
    bjson_t json = {.buf = line, .len = sizeof(line) - 1U};
    bjson_val_t root;
    bjson_val_t options;
    bjson_val_t item;
    bjson_val_t label;
    char text[32];

    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "options", &options));
    assert(bjson_arr_len(&options) == 2);
    assert(!bjson_arr_get(&options, 2, &item));

    assert(bjson_arr_get(&options, 1, &item));
    assert(bjson_obj_get(&item, "label", &label));
    assert(strcmp(str_of(&label, text, sizeof(text)), "拒绝") == 0);
    assert(bjson_obj_get(&item, "value", &label));
    assert(strcmp(str_of(&label, text, sizeof(text)), "deny") == 0);
}

static void test_rejects_garbage(void)
{
    static const char truncated[] = "{\"t\":\"state\"";
    static const char trailing[] = "{\"t\":\"state\"} extra";
    static const char not_json[] = "I (1234) wifi: state: run -> init";
    static const char bad_escape[] = "{\"a\":\"\\q\"}";
    static const char bad_hex[] = "{\"a\":\"\\uZZZZ\"}";
    bjson_t json;
    bjson_val_t root;

    json.buf = truncated;
    json.len = sizeof(truncated) - 1U;
    assert(!bjson_parse(&json, &root));

    json.buf = trailing;
    json.len = sizeof(trailing) - 1U;
    assert(!bjson_parse(&json, &root));

    json.buf = "";
    json.len = 0U;
    assert(!bjson_parse(&json, &root));

    // 这条是设备控制台日志的样子：解析器必须干脆地拒掉它，而不是猜。
    json.buf = not_json;
    json.len = sizeof(not_json) - 1U;
    assert(!bjson_parse(&json, &root));

    // 坏转义**不是**解析失败：扫描只看结构（一段字符串到哪里结束），不解释里面的
    // 内容。它在这取的那一刻暴露——解出空串，而不是让整条消息作废。
    json.buf = bad_escape;
    json.len = sizeof(bad_escape) - 1U;
    assert(bjson_parse(&json, &root));
    {
        bjson_val_t value;
        char text[8] = "xxxx";

        assert(bjson_obj_get(&root, "a", &value));
        assert(bjson_str_into(&value, text, sizeof(text), NULL) == 0U);
        assert(text[0] == '\0');
    }

    json.buf = bad_hex;
    json.len = sizeof(bad_hex) - 1U;
    assert(bjson_parse(&json, &root));
    {
        bjson_val_t value;
        char text[8] = "xxxx";

        assert(bjson_obj_get(&root, "a", &value));
        assert(bjson_str_into(&value, text, sizeof(text), NULL) == 0U);
        assert(text[0] == '\0');
    }

    // 值不是对象时，按键取值只是失败，不是崩溃。
    json.buf = "123";
    json.len = 3U;
    assert(bjson_parse(&json, &root));
    assert(root.kind == BJSON_INT);
    assert(!bjson_obj_get(&root, "t", &root));
    assert(bjson_arr_len(&root) == 0U);
}

// 截断只能落在字符边界上：按字节砍中文会得到乱码，而那看起来像是字体坏了。
static void test_truncates_on_character_boundary(void)
{
    static const char line[] = "{\"s\":\"中文标题 ABC\"}";
    bjson_t json = {.buf = line, .len = sizeof(line) - 1U};
    bjson_val_t root;
    bjson_val_t value;
    char text[64];

    assert(bjson_parse(&json, &root));
    assert(bjson_obj_get(&root, "s", &value));

    // 中文一个字三字节：cap 刚好放进「中」加 NUL，装不下「文」。
    assert(bjson_str_into(&value, text, 4U, NULL) == 3U);
    assert(strcmp(text, "中") == 0);

    // 连一个字符都放不下时留空，而不是留半个字符。
    assert(bjson_str_into(&value, text, 3U, NULL) == 0U);
    assert(text[0] == '\0');

    // 够长时完整拷贝。
    assert(bjson_str_into(&value, text, sizeof(text), NULL) == strlen("中文标题 ABC"));
    assert(strcmp(text, "中文标题 ABC") == 0);
}

// 设备回传的答案要用问题全文当键，那条路径上的转义必须能走一个来回。
static void test_escape_round_trip(void)
{
    static const char *const samples[] = {
        "hello",
        "中文标题",
        "quote\"and\\backslash",
        "brace}and]bracket",
        "tab\tnewline\nend",
        "control\x01char",
    };
    size_t i;

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
        char encoded[256];
        bjson_t json;
        bjson_val_t root;
        char decoded[256];

        assert(bjson_escape(encoded, sizeof(encoded), samples[i]) > 0);
        json.buf = encoded;
        json.len = strlen(encoded);
        assert(bjson_parse(&json, &root));
        assert(root.kind == BJSON_STRING);
        assert(strcmp(str_of(&root, decoded, sizeof(decoded)), samples[i]) == 0);
    }

    // 缓冲不够时什么都不写，而不是写半个字面量。
    {
        char small[4];
        memset(small, 'x', sizeof(small));
        assert(bjson_escape(small, sizeof(small), "abcdef") == 0);
        assert(small[0] == 'x');
    }
}

int main(void)
{
    test_object_fields();
    test_escapes();
    test_braces_inside_strings();
    test_arrays_of_objects();
    test_rejects_garbage();
    test_truncates_on_character_boundary();
    test_escape_round_trip();
    printf("test_badge_json: OK\n");
    return 0;
}
