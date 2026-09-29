#include "badge_voice.h"

#include <string.h>

// 一个只会追加的写出缓冲。任何一次越界都让**整条消息**作废（ok = false）：宁可让调用方
// 丢掉这一条，也不要发出半条——半条 JSON 在主机那边是一行永远解析不了的东西，而发方
// 这边一切正常（badge_ble.h 顶上那段说的就是这件事）。
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
    w->len = 0U;
    w->ok = (out != NULL && cap > 0U);
    if (w->ok) {
        out[0] = '\0';
    }
}

// writer_raw 追加 length 个字节。`>=` 而不是 `>`：结尾那个 NUL 也要占一格。
static void writer_raw(writer_t *w, const char *text, size_t length)
{
    if (!w->ok) {
        return;
    }
    if (w->len + length >= w->cap) {
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
    char digits[24];
    size_t length = 0U;
    size_t i;

    if (value == 0UL) {
        writer_raw(w, "0", 1U);
        return;
    }
    while (value > 0UL && length < sizeof(digits)) {
        digits[length++] = (char)('0' + (int)(value % 10UL));
        value /= 10UL;
    }
    for (i = 0U; i < length / 2U; ++i) {
        const char swap = digits[i];

        digits[i] = digits[length - 1U - i];
        digits[length - 1U - i] = swap;
    }
    writer_raw(w, digits, length);
}

static void writer_invalidate(writer_t *w)
{
    w->ok = false;
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

// base64 的字母表。**标准**那一张（含 '+' 与 '/'）：协议两端各自实现编解码，谁都
// 不该有自己的一张表——一张 URL-safe 的表在这里只会让主机那边多一个不匹配的机会。
static const char B64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t badge_voice_base64(char *out, size_t cap, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t need;
    size_t written = 0U;
    size_t i;

    if (out == NULL || cap == 0U) {
        return 0U;
    }
    if (bytes == NULL && length > 0U) {
        return 0U;
    }
    // 4 个字符一组，末尾不满 3 字节时补 '='。先算总长再写：写一半才发现装不下，
    // 留下的就是一个半截的 base64。
    need = ((length + 2U) / 3U) * 4U;
    if (need + 1U > cap) {
        out[0] = '\0';
        return 0U;
    }

    for (i = 0U; i + 3U <= length; i += 3U) {
        const unsigned value = ((unsigned)bytes[i] << 16) |
                               ((unsigned)bytes[i + 1U] << 8) |
                               (unsigned)bytes[i + 2U];

        out[written++] = B64_ALPHABET[(value >> 18) & 0x3FU];
        out[written++] = B64_ALPHABET[(value >> 12) & 0x3FU];
        out[written++] = B64_ALPHABET[(value >> 6) & 0x3FU];
        out[written++] = B64_ALPHABET[value & 0x3FU];
    }
    if (i < length) {
        // 尾部的 1 或 2 个字节：低位补零，右边补 '='。
        const bool two = (i + 1U) < length;
        unsigned value = (unsigned)bytes[i] << 16;

        if (two) {
            value |= (unsigned)bytes[i + 1U] << 8;
        }
        out[written++] = B64_ALPHABET[(value >> 18) & 0x3FU];
        out[written++] = B64_ALPHABET[(value >> 12) & 0x3FU];
        out[written++] = two ? B64_ALPHABET[(value >> 6) & 0x3FU] : '=';
        out[written++] = '=';
    }
    out[written] = '\0';
    return written;
}

size_t badge_voice_begin(char *out, size_t cap, unsigned long id, size_t bytes)
{
    writer_t w;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"voice_begin\",\"id\":");
    writer_ulong(&w, id);
    writer_literal(&w, ",\"codec\":\"" BADGE_VOICE_CODEC "\",\"hz\":");
    writer_ulong(&w, (unsigned long)BADGE_VOICE_HZ);
    writer_literal(&w, ",\"bytes\":");
    writer_ulong(&w, (unsigned long)bytes);
    writer_literal(&w, "}");
    return writer_finish(&w);
}

size_t badge_voice_chunk(char *out, size_t cap, unsigned long id, const void *raw, size_t raw_length)
{
    writer_t w;
    size_t encoded;

    // 空片没有意义；超过一片上限的输入是调用方的 bug。两种都当作「编不出来」，
    // 而不是悄悄截断——截断会让主机收到少一截的音频，而两端都看不出来。
    if (raw_length == 0U || raw_length > BADGE_VOICE_RAW_CHUNK) {
        return 0U;
    }
    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"voice_chunk\",\"id\":");
    writer_ulong(&w, id);
    writer_literal(&w, ",\"data\":\"");
    // base64 直接写进 out 的尾部：中间不借第二块缓冲，因为这块板没有 PSRAM，
    // 而每片一行意味着一秒里要编十几次。
    encoded = badge_voice_base64(w.ok ? w.out + w.len : NULL, w.ok ? w.cap - w.len : 0U,
                                 raw, raw_length);
    if (encoded == 0U) {
        writer_invalidate(&w);
        return writer_finish(&w);
    }
    w.len += encoded;
    writer_literal(&w, "\"}");
    return writer_finish(&w);
}

size_t badge_voice_end(char *out, size_t cap, unsigned long id, size_t parts)
{
    writer_t w;

    writer_init(&w, out, cap);
    writer_literal(&w, "{\"t\":\"voice_end\",\"id\":");
    writer_ulong(&w, id);
    writer_literal(&w, ",\"parts\":");
    writer_ulong(&w, (unsigned long)parts);
    writer_literal(&w, "}");
    return writer_finish(&w);
}
