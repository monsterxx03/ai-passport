// badge_voice —— 语音上传（设备 → 主机）那一半。
//
// 一次上传是三条消息：voice_begin（先报总量）→ voice_chunk × N（每片 512 字节原始
// 数据的 base64）→ voice_end（报片数）。为什么分片而不是一条大行，理由在 tachi 那份
// 设计文档 §5：一条 64 KB 的行要求双方一起把它捧在内存里，中途失败就整条重来；分片
// 之后丢一片只损失一片，而且**进度可见**（屏幕上能显示「已发 3.1/5.0 秒」）。
//
// 这一层是**纯逻辑**：把字节编成协议行，不碰 ESP-IDF、不碰 LVGL、不碰音频驱动，
// 也不分配内存——所以它能在主机上被完整测试（tests/test_badge_voice.c）。真机上最难
// 查的正是编码本身：base64 的补位、最后一片不满、缓冲正好差一个字节。这些在主机上
// 都能试出来，而烧一次板子的代价比它大得多。
//
// 现在是 P0 的样子：数据源是一段**假装的**音频（见 main.c 的吐数据任务），而路径
// （分片 → base64 → send_line → BLE notify）与将来从麦克风采到的完全一样。所以它量
// 出来的吞吐就是语音上传的吞吐。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 每片装多少**原始**字节。512 是设计定的：base64 之后 684 字节，加 JSON 骨架约 730
// 字节一行，而 BLE 一次 notify 最多 MTU-3（实测 253）——一行就是三四个 notify。
#define BADGE_VOICE_RAW_CHUNK 512U

// 一行 voice_chunk 的上限：base64 膨胀是 **ceil(n/3)×4**（512 → 684），再加 JSON
// 骨架、id 字段和 NUL 的余量。给得宽一点不是浪费——一块 750 字节的缓冲在 8 MB flash
// 的板子上不算什么，而算少了的后果是「最后一片总是发不出去」这种只在真机上才看得见
// 的现象。数学上够不够，由 tests/test_badge_voice.c 钉住（不满一片的尾巴最容易算错）。
#define BADGE_VOICE_LINE_MAX (((BADGE_VOICE_RAW_CHUNK + 2U) / 3U) * 4U + 64U)

// 协议里报的 codec 与采样率。P0 的假数据还没有真编码，但字段按**最终形态**写：主机
// 侧收到的字段和将来一模一样，它那边的解析就不必再改第二遍。
#define BADGE_VOICE_CODEC "ima-adpcm"
#define BADGE_VOICE_HZ 16000U

// 下面几个把一条消息编进 out，返回写出的字节数（不含结尾 NUL）；cap 不够返回 0，
// 此时 out 里**不会有半个消息**——调用方据此丢掉这一条，而不是发出半条。
// 编码后的行不含 `@@` 前缀与换行：那是传输层的事（与 badge_proto 一致）。
size_t badge_voice_begin(char *out, size_t cap, unsigned long id, size_t bytes);
size_t badge_voice_chunk(char *out, size_t cap, unsigned long id, const void *raw, size_t raw_length);
size_t badge_voice_end(char *out, size_t cap, unsigned long id, size_t parts);

// badge_voice_base64 把 length 个字节编码成 base64，返回写出的字符数（不含 NUL）；
// cap 不够返回 0。单独暴露出来是为了能直接钉住补位规则——这是整条链上最容易错、
// 而错了又最难从现象上看出来的一块（少一个 '=' 主机那边就是一行解析不了的东西）。
size_t badge_voice_base64(char *out, size_t cap, const void *data, size_t length);

// --- 录音（设备 → 主机的那一段）------------------------------------------------
//
// 一次录音：唤醒 codec → 按帧读麦克风 → ADPCM 编码 → 攒成 512 字节的片 → 发
// voice_chunk → 收尾（flush + voice_end）→ 把 codec 睡回去。整流在自己的任务里，
// 因为 bsp_audio_read 会阻塞（BSP 的硬规矩：PCM 读写必须待在 worker task 里）。

// 采集上限。到点自动停并发送——不让人录到链路塞满才发现。30 秒 = 240 KB（16 kHz
// ADPCM 是 8 KB/s），按实测 13 KB/s 的链路算约 18 秒传完——而且它是**边录边传**的，
// 所以按下结束之后剩下的通常只有最后几片。
#define BADGE_VOICE_MAX_MS 30000U
// 采样率。ADPCM 是 4 bit/采样，所以 16 kHz 对应 8 KB/s——设计文档 §4 量出来的首选档
// （8 kHz 也够带宽，但短句上的英文词会塌成汉字，见那张表）。
#define BADGE_VOICE_HZ 16000U
// 一次录音最多能出多少**原始**（ADPCM）字节。它是 voice_begin 里报的那个数——那边报的
// 是**上限**：录音多长要到结束才知道，而主机拿这个数挡「超过上限的东西」。
#define BADGE_VOICE_MAX_BYTES (BADGE_VOICE_MAX_MS * (BADGE_VOICE_HZ / 1000U) / 2U)
// 每次从麦克风读多少采样（32 ms @ 16 kHz）。它是「一帧」，不是「一片」——两帧才凑成
// 一片 512 字节的 ADPCM（4 bit/采样 → 512 采样 = 256 字节）。
#define BADGE_VOICE_FRAME_SAMPLES 512U

// 一行怎么发出去由调用方给（main.c 的 send_line 知道该走串口还是 BLE）。
typedef bool (*badge_voice_send_fn)(const char *line, size_t length, void *context);

// 一次按键（主屏长按确定）做完的那件事。
typedef enum {
    BADGE_VOICE_ACTION_STARTED = 0, // 开始录
    BADGE_VOICE_ACTION_STOPPED,     // 提前结束，正在发
    BADGE_VOICE_ACTION_BUSY,        // 上一次还在发（或 codec 被提示音占着）
    BADGE_VOICE_ACTION_NO_AUDIO,    // 音频没起来，录不了
} badge_voice_action_t;

// 一次录音是怎么结束的。
typedef enum {
    BADGE_VOICE_OK = 0,       // 录完并发出去了（片数可能在 parts_failed 里）
    BADGE_VOICE_NO_AUDIO,     // codec 起不来
    BADGE_VOICE_SEND_FAILED,  // 连 begin/end 都发不出去（链路不在）
    BADGE_VOICE_CANCELLED,    // 录到一半被丢弃：已经发出去的片作废，主机那边也作废
} badge_voice_outcome_t;

typedef struct {
    badge_voice_outcome_t outcome;
    unsigned parts_sent;   // 成功送出去的片数
    unsigned parts_failed; // 失败的片数（主机按 parts 能看出少了）
    unsigned bytes;        // 送出去的原始（ADPCM）字节
    uint32_t ms;           // 实际采到的时长（按采到的采样数算，不受调度抖动影响）
    // 整段的峰值幅度（0..32767）。它不是给用户看的，是给**第一次上真机**用的：
    // 「片数与字节数都对」也可能是整段静音（麦克风没通、增益为 0），而那种失败在数字
    // 上看不出来——只有这个数会说。
    uint16_t peak;
} badge_voice_report_t;

// badge_voice_init 记住出口并准备一次录音所需的状态（app_main 调一次）。
void badge_voice_init(badge_voice_send_fn send, void *context);

// badge_voice_toggle 一次按键：空闲就开始录，录着就停并把这段发上去。
badge_voice_action_t badge_voice_toggle(void);

// badge_voice_take_report 取走一次录音的结果（由 app 任务调，取到就显示在屏幕上）。
// 采集与编码跑在别的任务里，所以结果只**在这里**交给状态机——状态机只由 app 任务碰。
bool badge_voice_take_report(badge_voice_report_t *out);

// badge_voice_busy 是否正在用麦克风/codec。提示音那条靠它避开：录音时来一声提示音，
// 两边会互踩 codec 的格式（BSP 的约定：调用方须串行化格式与休眠）。
bool badge_voice_busy(void);

// badge_voice_progress 报出「正在录、录了多久、上限多少」，给屏幕上的倒计时用。
// 没在录时返回 false（此时两个出参不动）。
//
// 为什么由它来答而不是让界面自己算：录了多久只有采集那条任务知道（它按采到的采样数
// 算时间，不受调度抖动影响），而屏幕在 app 任务那边。
bool badge_voice_progress(uint32_t *elapsed_ms, uint32_t *limit_ms);

// badge_voice_cancel 丢弃正在录的这一段（按键路径调）。录到的东西一个字节都不发：
// 主机那边已经在飞的片会因为收不到完整的 end 而作废（它按片数对账）。
//
// 没在录时是个空操作——这样调用方不必先问再调。
void badge_voice_cancel(void);
