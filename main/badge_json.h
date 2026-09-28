// badge_json —— 协议用的极简 JSON 读写。
//
// 为什么不直接用 cJSON：解析器要能在 host test 里跑（仓库的门禁用 `cc` 直接编译
// 纯逻辑文件），而 ESP-IDF 自带的 cJSON 在主机上没有。写成这样一份零依赖的代码，
// 设备与测试跑的是同一个实现，不会出现「测试绿了、真机解析不同」。
//
// 它只覆盖协议用到的子集：对象、数组、字符串、整数、布尔、null。没有浮点，
// **没有动态分配**——这块板没有 PSRAM，一次解析的中间态不该去碰堆。
//
// 解析是**只读**的：它绝不改写输入。这不是洁癖，是遍历方式的要求——容器里的元素
// 靠反复从头扫来找（先扫到目标 key 之前要跳过一串兄弟值），一旦解析就地解码，
// 第一次遍历就把引号覆盖掉了，第二次遍历同一个值就再也认不出来。
//
// 所以字符串以「原文范围」的形式给出，取值时用 bjson_str_into 解码到调用方的
// 缓冲里——那里的长度和 UTF-8 边界由它负责，业务结构本来也要拷一份。
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    BJSON_NONE = 0,
    BJSON_OBJECT,
    BJSON_ARRAY,
    BJSON_STRING,
    BJSON_INT,
    BJSON_BOOL,
    BJSON_NULL,
} bjson_kind_t;

// bjson_t 是被解析的那一行。它只读，不会被改写。
typedef struct {
    const char *buf;
    size_t len;
} bjson_t;

// bjson_val_t 指向缓冲里的一个值。字符串记的是**未解码**的内容范围
// [text, text_end)，取值时交给 bjson_str_into。
typedef struct {
    bjson_kind_t kind;
    const char *start;    // 值起点
    const char *end;      // 值结束（开区间）
    const char *text;     // BJSON_STRING：内容起点（不含引号）
    const char *text_end; // BJSON_STRING：内容终点（不含引号）
    long number;          // BJSON_INT
    bool boolean;         // BJSON_BOOL
} bjson_val_t;

// bjson_parse 解析整行。失败返回 false——调用方把这一行丢掉即可：协议行来自
// 一条混着控制台日志的串口，坏数据是常态而不是异常。
bool bjson_parse(bjson_t *j, bjson_val_t *out);

// bjson_obj_get 在对象里按键取值。找不到、或不是对象，返回 false。
bool bjson_obj_get(const bjson_val_t *obj, const char *key, bjson_val_t *out);

// bjson_arr_len / bjson_arr_get 遍历数组。数组小（协议里最多几个元素），
// 所以按下标取是线性扫——不为此维护游标。
size_t bjson_arr_len(const bjson_val_t *arr);
bool bjson_arr_get(const bjson_val_t *arr, size_t index, bjson_val_t *out);

// bjson_str_into 把字符串解码写进 dst（NUL 结尾），返回写入的字节数。
//
// full_length（可以为 NULL）拿到**解码后的完整长度**：它大于返回值就说明发生了
// 截断。需要这个区分是因为有些字段被截断后语义就变了——比如 AskUser 的问题全文，
// 它是回传答案的键，截一半就等于答非所问，而这种错误在设备上看起来一切正常。
//
// 截断落在 UTF-8 字符边界上，绝不留半个字符——按字节砍中文会得到乱码，而那正是
// 「字体没问题、字却是碎的」那类排查的死结。
size_t bjson_str_into(const bjson_val_t *v, char *dst, size_t cap, size_t *full_length);

// 取值助手：类型不符时返回兜底值，不报错。协议里少一个字段只是少一条信息，
// 不值得让整条消息作废。
long bjson_int(const bjson_val_t *v, long fallback);
bool bjson_bool(const bjson_val_t *v, bool fallback);

// bjson_escape 把一个 NUL 结尾的字符串写成 JSON 字符串字面量（含两侧引号）。
// 返回写出的字节数（不含结尾 NUL）；cap 不够时返回 0，且不写半个字面量。
size_t bjson_escape(char *dst, size_t cap, const char *src);
