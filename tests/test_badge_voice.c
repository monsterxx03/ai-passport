// badge_voice 的单元测试。
//
// 编码这一层错起来最贵：base64 少一个 '=' 、或者一行装不下最后一片，现象都是「主机那边
// 什么都没收到」——而设备这边完全正常（它确实把字节交给了传输层）。所以这里钉的是
// 补位规则、缓冲边界、以及「常量到底够不够」这三件事。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "badge_voice.h"

static char line[BADGE_VOICE_LINE_MAX + 4096U];

// 已知向量：RFC 4648 的那几个。补位错一位，主机解出来的音频就整段是垃圾，
// 而两端的日志都不会说一句话。
static void test_base64_vectors(void)
{
    assert(badge_voice_base64(line, sizeof(line), "", 0U) == 0U);
    assert(strcmp(line, "") == 0);

    assert(badge_voice_base64(line, sizeof(line), "f", 1U) == 4U);
    assert(strcmp(line, "Zg==") == 0);

    assert(badge_voice_base64(line, sizeof(line), "fo", 2U) == 4U);
    assert(strcmp(line, "Zm8=") == 0);

    assert(badge_voice_base64(line, sizeof(line), "foo", 3U) == 4U);
    assert(strcmp(line, "Zm9v") == 0);

    assert(badge_voice_base64(line, sizeof(line), "foob", 4U) == 8U);
    assert(strcmp(line, "Zm9vYg==") == 0);

    assert(badge_voice_base64(line, sizeof(line), "fooba", 5U) == 8U);
    assert(strcmp(line, "Zm9vYmE=") == 0);

    assert(badge_voice_base64(line, sizeof(line), "foobar", 6U) == 8U);
    assert(strcmp(line, "Zm9vYmFy") == 0);
}

// 全零与全 0xFF：字母表的下标与掩码写错时，这两组最容易露馅。
static void test_base64_bytes(void)
{
    static const unsigned char zeros[3] = {0x00, 0x00, 0x00};
    static const unsigned char ones[3] = {0xFF, 0xFF, 0xFF};

    assert(badge_voice_base64(line, sizeof(line), zeros, 3U) == 4U);
    assert(strcmp(line, "AAAA") == 0);
    assert(badge_voice_base64(line, sizeof(line), ones, 3U) == 4U);
    assert(strcmp(line, "////") == 0);
}

// 缓冲差一个字节也要整条作废：留下一个半截的 base64 比不发更坏——它在主机那边是
// 一行能解析、但解出半截音频的东西。
static void test_base64_refuses_short_buffer(void)
{
    char small[5];

    memset(small, 'x', sizeof(small));
    assert(badge_voice_base64(small, sizeof(small), "foob", 4U) == 0U); // 需要 8 + NUL
    assert(small[0] == '\0');

    assert(badge_voice_base64(line, sizeof(line), "foob", 4U) == 8U); // 正好够（9 格）
    assert(strcmp(line, "Zm9vYg==") == 0);
}

// 一片满 512 字节的行必须装得进 BADGE_VOICE_LINE_MAX。这条断言是那个常量的**唯一**
// 用途：算少了的表现是「最后一片永远发不出去」，而它在主机那边看起来像丢包。
static void test_full_chunk_fits_the_line(void)
{
    static const char prefix[] = "{\"t\":\"voice_chunk\",\"id\":7,\"data\":\"";
    static unsigned char raw[BADGE_VOICE_RAW_CHUNK];
    char tight[BADGE_VOICE_LINE_MAX];
    size_t i;
    size_t length;

    for (i = 0U; i < sizeof(raw); ++i) {
        raw[i] = (unsigned char)(i & 0xFFU);
    }
    length = badge_voice_chunk(tight, sizeof(tight), 7UL, raw, sizeof(raw));
    assert(length > 0U);
    assert(length < sizeof(tight));
    // 684 个 base64 字符 + 骨架。
    assert(length == 684U + strlen(prefix) + strlen("\"}"));
    assert(strncmp(tight, prefix, strlen(prefix)) == 0);
    assert(tight[length - 2U] == '"');
    assert(tight[length - 1U] == '}');
}

// 尾巴那一片不满是常态（64 KiB / 512 = 128 片正好整，但真实音频不会这么巧）。
static void test_partial_tail_chunk(void)
{
    static unsigned char raw[100];
    size_t length;

    memset(raw, 0x41, sizeof(raw)); // 'A' × 100
    length = badge_voice_chunk(line, sizeof(line), 1UL, raw, sizeof(raw));
    assert(length > 0U);
    // 100 → ceil(100/3)×4 = 136 个 base64 字符（末尾 "QUE=" 式的补位）。
    assert(strstr(line, "\"data\":\"") != NULL);
    assert(length == 136U + strlen("{\"t\":\"voice_chunk\",\"id\":1,\"data\":\"\"}"));
}

// 空片与超长片都是调用方的 bug：返回 0（而不是悄悄截断或发一条空数据），
// 让调用方丢掉这一条。
static void test_chunk_rejects_bad_lengths(void)
{
    static unsigned char raw[BADGE_VOICE_RAW_CHUNK + 1U];

    assert(badge_voice_chunk(line, sizeof(line), 1UL, raw, 0U) == 0U);
    assert(badge_voice_chunk(line, sizeof(line), 1UL, raw, sizeof(raw)) == 0U);
    assert(badge_voice_chunk(line, sizeof(line), 1UL, raw, BADGE_VOICE_RAW_CHUNK) > 0U);
}

static void test_begin_and_end(void)
{
    char text[256];

    // 用真实的上限来断言：那正是设备会报出去的那个数（30 秒 × 8 KB/s = 240000）。
    assert(badge_voice_begin(text, sizeof(text), 3UL, BADGE_VOICE_MAX_BYTES) > 0U);
    assert(strcmp(text, "{\"t\":\"voice_begin\",\"id\":3,\"codec\":\"ima-adpcm\","
                        "\"hz\":16000,\"bytes\":240000}") == 0);

    assert(badge_voice_end(text, sizeof(text), 3UL, 128U) > 0U);
    assert(strcmp(text, "{\"t\":\"voice_end\",\"id\":3,\"parts\":128}") == 0);
}

// 缓冲装不下时整条消息作废，不留半条。
static void test_messages_refuse_short_buffers(void)
{
    char tiny[16];

    memset(tiny, 'x', sizeof(tiny));
    assert(badge_voice_begin(tiny, sizeof(tiny), 3UL, BADGE_VOICE_MAX_BYTES) == 0U);
    assert(tiny[0] == '\0');
    assert(badge_voice_end(tiny, sizeof(tiny), 3UL, 128U) == 0U);
    assert(tiny[0] == '\0');
    assert(badge_voice_begin(tiny, 0U, 3UL, 0U) == 0U);
}

int main(void)
{
    test_base64_vectors();
    test_base64_bytes();
    test_base64_refuses_short_buffer();
    test_full_chunk_fits_the_line();
    test_partial_tail_chunk();
    test_chunk_rejects_bad_lengths();
    test_begin_and_end();
    test_messages_refuse_short_buffers();
    printf("test_badge_voice: OK\n");
    return 0;
}
