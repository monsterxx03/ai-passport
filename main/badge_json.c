#include "badge_json.h"

#include <stdio.h>
#include <string.h>

// 解析器把值当作一段 [start, end) 的范围，容器里的元素每次从头线性扫。协议里的
// 消息很小（几个会话、几个选项），这个复杂度不值得为它引入游标状态。整个过程
// **只读**，所以同一段范围可以被反复扫（见头文件的说明）。

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && is_space(*p)) {
        ++p;
    }
    return p;
}

// scan_raw_string 定位一段字符串字面量的内容范围，返回结束引号之后的位置。
// 它不解码，所以容器定界不会被字符串里的括号骗到（`{"a":"}"}`）。
static const char *scan_raw_string(const char *p, const char *end,
                                   const char **text, const char **text_end)
{
    if (p >= end || *p != '"') {
        return NULL;
    }
    *text = p + 1;
    ++p;
    while (p < end) {
        if (*p == '\\') {
            p += 2; // 转义序列至少两个字节；越过 end 由下一轮边界检查兜住
            continue;
        }
        if (*p == '"') {
            *text_end = p;
            return p + 1;
        }
        ++p;
    }
    return NULL;
}

// scan_container 从开括号走到配对的闭括号，返回闭括号之后的位置。
// 只配对自己这一对括号：内部的其他容器由它们各自的配对闭合，互不干扰。
static const char *scan_container(const char *p, const char *end, char open, char close)
{
    int depth = 0;

    while (p < end) {
        if (*p == '"') {
            const char *text;
            const char *text_end;

            p = scan_raw_string(p, end, &text, &text_end);
            if (p == NULL) {
                return NULL;
            }
            continue;
        }
        if (*p == open) {
            ++depth;
        } else if (*p == close) {
            --depth;
            if (depth == 0) {
                return p + 1;
            }
        }
        ++p;
    }
    return NULL;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool text_matches(const char *p, const char *end, const char *literal)
{
    size_t length = strlen(literal);

    return (size_t)(end - p) >= length && strncmp(p, literal, length) == 0;
}

// parse_value 解析一个值，返回它之后的位置；失败返回 NULL。
static const char *parse_value(const char *p, const char *end, bjson_val_t *out)
{
    memset(out, 0, sizeof(*out));
    if (p >= end) {
        return NULL;
    }
    out->start = p;

    switch (*p) {
    case '{': {
        const char *q = scan_container(p, end, '{', '}');
        if (q == NULL) {
            return NULL;
        }
        out->kind = BJSON_OBJECT;
        out->end = q;
        return q;
    }
    case '[': {
        const char *q = scan_container(p, end, '[', ']');
        if (q == NULL) {
            return NULL;
        }
        out->kind = BJSON_ARRAY;
        out->end = q;
        return q;
    }
    case '"': {
        const char *q = scan_raw_string(p, end, &out->text, &out->text_end);
        if (q == NULL) {
            return NULL;
        }
        out->kind = BJSON_STRING;
        out->end = q;
        return q;
    }
    case 't':
        if (text_matches(p, end, "true")) {
            out->kind = BJSON_BOOL;
            out->boolean = true;
            out->end = p + 4;
            return out->end;
        }
        return NULL;
    case 'f':
        if (text_matches(p, end, "false")) {
            out->kind = BJSON_BOOL;
            out->boolean = false;
            out->end = p + 5;
            return out->end;
        }
        return NULL;
    case 'n':
        if (text_matches(p, end, "null")) {
            out->kind = BJSON_NULL;
            out->end = p + 4;
            return out->end;
        }
        return NULL;
    default:
        break;
    }

    // 数字。自己扫而不是交给 strtol：这里没有 NUL 结尾可用（那一行不能被改写），
    // 而协议里的数字全是整数。
    {
        bool negative = false;
        long value = 0;
        const char *q = p;

        if (*q == '-') {
            negative = true;
            ++q;
        }
        if (q >= end || *q < '0' || *q > '9') {
            return NULL;
        }
        while (q < end && *q >= '0' && *q <= '9') {
            value = value * 10 + (long)(*q - '0');
            ++q;
        }
        // 小数与指数在这里没有意义：协议只传整数，遇到就到此为止。
        out->kind = BJSON_INT;
        out->number = negative ? -value : value;
        out->end = q;
        return q;
    }
}

// key_equals 比较一个键与字面量。键的解码是多余的（Go 生成的键名不会带转义），
// 但一个带转义的键被当成「找不到」会是一条很难查的静默路径，所以照解不误——
// 键很短，栈上开个小缓冲即可。
static bool key_equals(const bjson_val_t *key, const char *literal)
{
    char decoded[32];

    if (key->kind != BJSON_STRING) {
        return false;
    }
    (void)bjson_str_into(key, decoded, sizeof(decoded), NULL);
    return strcmp(decoded, literal) == 0;
}

bool bjson_parse(bjson_t *j, bjson_val_t *out)
{
    const char *p;
    const char *end;

    if (j == NULL || j->buf == NULL || out == NULL) {
        return false;
    }
    end = j->buf + j->len;
    p = skip_ws(j->buf, end);
    p = parse_value(p, end, out);
    if (p == NULL) {
        return false;
    }
    p = skip_ws(p, end);
    // 尾部只允许空白：一行里多出来的东西说明它不是我们以为的那条消息。
    return p == end;
}

bool bjson_obj_get(const bjson_val_t *obj, const char *key, bjson_val_t *out)
{
    const char *p;
    const char *end;

    if (obj == NULL || obj->kind != BJSON_OBJECT || key == NULL || out == NULL) {
        return false;
    }
    end = obj->end - 1; // 收尾的 '}'
    p = skip_ws(obj->start + 1, end);

    while (p < end && *p != '}') {
        bjson_val_t name;
        bjson_val_t value;

        p = parse_value(p, end, &name);
        if (p == NULL || name.kind != BJSON_STRING) {
            return false;
        }
        p = skip_ws(p, end);
        if (p >= end || *p != ':') {
            return false;
        }
        p = parse_value(skip_ws(p + 1, end), end, &value);
        if (p == NULL) {
            return false;
        }
        if (key_equals(&name, key)) {
            *out = value;
            return true;
        }
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
        }
    }
    return false;
}

size_t bjson_arr_len(const bjson_val_t *arr)
{
    const char *p;
    const char *end;
    size_t count = 0;

    if (arr == NULL || arr->kind != BJSON_ARRAY) {
        return 0;
    }
    end = arr->end - 1; // 收尾的 ']'
    p = skip_ws(arr->start + 1, end);

    while (p < end && *p != ']') {
        bjson_val_t value;

        p = parse_value(p, end, &value);
        if (p == NULL) {
            return 0;
        }
        ++count;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
        }
    }
    return count;
}

bool bjson_arr_get(const bjson_val_t *arr, size_t index, bjson_val_t *out)
{
    const char *p;
    const char *end;
    size_t current = 0;

    if (arr == NULL || arr->kind != BJSON_ARRAY || out == NULL) {
        return false;
    }
    end = arr->end - 1;
    p = skip_ws(arr->start + 1, end);

    while (p < end && *p != ']') {
        p = parse_value(p, end, out);
        if (p == NULL) {
            return false;
        }
        if (current == index) {
            return true;
        }
        ++current;
        p = skip_ws(p, end);
        if (p < end && *p == ',') {
            p = skip_ws(p + 1, end);
        }
    }
    return false;
}

long bjson_int(const bjson_val_t *v, long fallback)
{
    if (v == NULL || v->kind != BJSON_INT) {
        return fallback;
    }
    return v->number;
}

bool bjson_bool(const bjson_val_t *v, bool fallback)
{
    if (v == NULL || v->kind != BJSON_BOOL) {
        return fallback;
    }
    return v->boolean;
}

// utf8_length 由首字节给出这个字符占几个字节；非法首字节返回 1（当作单字节跳过，
// 这样一段坏字节最多产生一个坏字符，而不是把后面的内容也吞掉）。
static size_t utf8_length(unsigned char c)
{
    if (c < 0x80U) {
        return 1U;
    }
    if ((c & 0xE0U) == 0xC0U) {
        return 2U;
    }
    if ((c & 0xF0U) == 0xE0U) {
        return 3U;
    }
    if ((c & 0xF8U) == 0xF0U) {
        return 4U;
    }
    return 1U;
}

// decode_escape 解码一个反斜杠序列，写进 out（最多 3 字节），返回输出长度和
// 序列之后的位置；失败返回 0。代理对不做处理：协议的 JSON 由 Go 的 encoding/json
// 生成，它只把 < > & 和控制字符写成 \uXXXX，都是 BMP 内的单码点。
static size_t decode_escape(const char *p, const char *end, char out[3], const char **next)
{
    char c;

    if (p >= end || *p != '\\') {
        return 0;
    }
    ++p;
    if (p >= end) {
        return 0;
    }
    c = *p++;
    switch (c) {
    case '"':
    case '\\':
    case '/':
        out[0] = c;
        *next = p;
        return 1U;
    case 'b':
        out[0] = '\b';
        *next = p;
        return 1U;
    case 'f':
        out[0] = '\f';
        *next = p;
        return 1U;
    case 'n':
        out[0] = '\n';
        *next = p;
        return 1U;
    case 'r':
        out[0] = '\r';
        *next = p;
        return 1U;
    case 't':
        out[0] = '\t';
        *next = p;
        return 1U;
    case 'u': {
        unsigned codepoint = 0U;
        int i;

        if (end - p < 4) {
            return 0;
        }
        for (i = 0; i < 4; ++i) {
            int digit = hex_digit(p[i]);

            if (digit < 0) {
                return 0;
            }
            codepoint = (codepoint << 4) | (unsigned)digit;
        }
        if (codepoint < 0x80U) {
            out[0] = (char)codepoint;
            *next = p + 4;
            return 1U;
        }
        if (codepoint < 0x800U) {
            out[0] = (char)(0xC0U | (codepoint >> 6));
            out[1] = (char)(0x80U | (codepoint & 0x3FU));
            *next = p + 4;
            return 2U;
        }
        out[0] = (char)(0xE0U | (codepoint >> 12));
        out[1] = (char)(0x80U | ((codepoint >> 6) & 0x3FU));
        out[2] = (char)(0x80U | (codepoint & 0x3FU));
        *next = p + 4;
        return 3U;
    }
    default:
        return 0;
    }
}

size_t bjson_str_into(const bjson_val_t *v, char *dst, size_t cap, size_t *full_length)
{
    const char *p;
    size_t written = 0;
    size_t decoded = 0; // 解码后的完整长度，不管装不装得下
    bool truncated = false;

    if (full_length != NULL) {
        *full_length = 0;
    }
    if (dst != NULL && cap > 0U) {
        dst[0] = '\0';
    }
    if (v == NULL || v->kind != BJSON_STRING || v->text == NULL || dst == NULL) {
        return 0;
    }

    p = v->text;
    while (p < v->text_end) {
        char buffer[4];
        size_t length;

        if (*p == '\\') {
            const char *next = NULL;

            length = decode_escape(p, v->text_end, buffer, &next);
            if (length == 0U) {
                break; // 坏转义：在这里收尾，已经写进去的部分仍然有效
            }
            p = next;
        } else {
            length = utf8_length((unsigned char)*p);
            if ((size_t)(v->text_end - p) < length) {
                break; // 结尾是半个字符，丢掉而不是写出乱码
            }
            memcpy(buffer, p, length);
            p += length;
        }

        decoded += length;
        // 装不下就**停止写入**（不是跳过这个字符接着写后面的——那会在文本中间
        // 挖出一个洞，比如中文放不下而空格和字母挤了进来），但继续数下去，
        // 这样 full_length 仍是完整的长度，调用方能据它判断确实被截断了。
        if (!truncated) {
            if (written + length + 1U > cap) {
                truncated = true;
            } else {
                memcpy(dst + written, buffer, length);
                written += length;
            }
        }
    }
    dst[written] = '\0';
    if (full_length != NULL) {
        *full_length = decoded;
    }
    return written;
}

// escape_width 给出一个字节在 JSON 字面量里占几个字节。
static size_t escape_width(unsigned char c)
{
    switch (c) {
    case '"':
    case '\\':
    case '\n':
    case '\r':
    case '\t':
    case '\b':
    case '\f':
        return 2U;
    default:
        return c < 0x20U ? 6U : 1U; // \u00xx
    }
}

size_t bjson_escape(char *dst, size_t cap, const char *src)
{
    size_t needed = 3U; // 两个引号 + 结尾 NUL
    size_t written = 0;
    const char *p;

    if (dst == NULL || src == NULL) {
        return 0;
    }
    // 先量后写。边写边查看着更省事，但失败时前面那几个字节已经落进调用方的缓冲里了
    // ——「要么完整，要么什么都不写」是调用方唯一能安全依赖的承诺。
    for (p = src; *p != '\0'; ++p) {
        needed += escape_width((unsigned char)*p);
    }
    if (needed > cap) {
        return 0;
    }

    dst[written++] = '"';
    for (p = src; *p != '\0'; ++p) {
        unsigned char c = (unsigned char)*p;

        switch (c) {
        case '"':
            dst[written++] = '\\';
            dst[written++] = '"';
            break;
        case '\\':
            dst[written++] = '\\';
            dst[written++] = '\\';
            break;
        case '\n':
            dst[written++] = '\\';
            dst[written++] = 'n';
            break;
        case '\r':
            dst[written++] = '\\';
            dst[written++] = 'r';
            break;
        case '\t':
            dst[written++] = '\\';
            dst[written++] = 't';
            break;
        case '\b':
            dst[written++] = '\\';
            dst[written++] = 'b';
            break;
        case '\f':
            dst[written++] = '\\';
            dst[written++] = 'f';
            break;
        default:
            if (c < 0x20U) {
                static const char hex[] = "0123456789abcdef";

                dst[written++] = '\\';
                dst[written++] = 'u';
                dst[written++] = '0';
                dst[written++] = '0';
                dst[written++] = hex[(c >> 4) & 0x0FU];
                dst[written++] = hex[c & 0x0FU];
            } else {
                dst[written++] = (char)c;
            }
            break;
        }
    }
    dst[written++] = '"';
    dst[written] = '\0';
    return written;
}
