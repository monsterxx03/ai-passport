// badge_voice_task —— 语音上传的**设备侧那一端**：按键 → 采集 → 编码 → 发送。
//
// 与 badge_voice.c 的分工：那个文件是**纯逻辑**（消息怎么编、base64 怎么补位），能在主机上
// 完整测试；这个文件是 I/O（BSP 的麦克风、FreeRTOS 的任务与延时、ESP-IDF 的日志），只能
// 在真机上验。仓库里每一对「协议/状态机 vs 传输」都是这么分的（badge_proto vs badge_link，
// badge_state vs badge_ui），原因也一样：真机上最难查的错，多半能在主机上试出来。
#include "badge_voice.h"

// --- 录音 ---------------------------------------------------------------------

#include "badge_adpcm.h"
#include "badge_sound.h" // badge_sound_busy：提示音在放的时候不许开始录音（codec 互斥）
#include "bsp_audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "badge_voice";

// 这个任务的栈：它只做采集与编码，两个缓冲都是静态的，所以 4096 够。优先级与提示音
// 的播放任务同级——两者都直接对着 codec 的 DMA 节奏，谁也不该被界面（5）或链路（3）
// 拖住。
#define VOICE_TASK_STACK 4096U
#define VOICE_TASK_PRIORITY 4U

static badge_voice_send_fn s_send;
static void *s_send_context;
static volatile bool s_busy;      // 正在录或正在发（提示音那条靠它让路）
static volatile bool s_recording; // 还在采集（区别于「正在发」）
static volatile bool s_stop;      // 有人请求提前结束
static volatile bool s_cancel;    // 有人请求丢弃这一段（按键那条）
// 这一轮采集是什么时候开始的（毫秒）。屏幕的倒计时按它算——所以它是 volatile：
// 写它的是采集任务，读它的是 app 任务。
static volatile uint32_t s_record_started_ms;
static volatile bool s_report_ready;
static badge_voice_report_t s_report;
static unsigned long s_voice_id;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool send_line(const char *line, size_t length)
{
    if (s_send == NULL || line == NULL || length == 0U) {
        return false;
    }
    return s_send(line, length, s_send_context);
}

// send_part 编一片并交出去。失败只记数、不中止整次上传：主机按 end 里的总片数能看出
// 少了多少，而丢一片比整段重录对用户更友好（设计文档 §8）。
static void send_part(unsigned long id, const uint8_t *raw, size_t length,
                      unsigned *sent, unsigned *failed, size_t *bytes)
{
    static char line[BADGE_VOICE_LINE_MAX]; // 静态：这个任务的栈放不下一行 750 字节
    const size_t encoded = badge_voice_chunk(line, sizeof(line), id, raw, length);

    if (encoded == 0U || !send_line(line, encoded)) {
        ++(*failed);
        return;
    }
    ++(*sent);
    // 按**实际**长度累加，而不是片数 × 512：最后一片通常只有半片（18 秒的录音里
    // 那是一次 256 字节的差别，而日志上那两个数不一致会让人怀疑是不是丢了数据）。
    *bytes += length;
}

static void voice_task(void *argument)
{
    static int16_t pcm[BADGE_VOICE_FRAME_SAMPLES];
    static uint8_t adpcm[BADGE_VOICE_RAW_CHUNK];
    badge_adpcm_enc_t enc;
    badge_voice_report_t report = {0};
    const uint32_t started_ms = now_ms();
    unsigned long id;
    size_t filled = 0U;
    size_t frames = 0U;
    unsigned sent = 0U;
    unsigned failed = 0U;
    size_t bytes_sent = 0U;
    uint16_t peak = 0U;
    bool begun = false;

    (void)argument;

    report.outcome = BADGE_VOICE_OK;
    // codec 平时睡着（提示音那条留下的约定），这里唤醒并设成录音要的格式。与提示音
    // 播放之间的互斥见 badge_voice_start 与 badge_sound 里那两处检查。
    if (bsp_audio_wake() != ESP_OK ||
        bsp_audio_set_format(BADGE_VOICE_HZ, 16U, 1U) != ESP_OK) {
        ESP_LOGW(TAG, "codec 起不来，这次录音作废");
        report.outcome = BADGE_VOICE_NO_AUDIO;
        goto done;
    }
    id = ++s_voice_id;
    {
        // begin 里的字节数是**上限**：录音多长要到结束才知道，而主机拿它挡「超过上限
        // 的东西」；「到底录了多长」由 end 的片数说了算。
        static char begin[128]; // 静态，理由同 send_part

        const size_t length = badge_voice_begin(begin, sizeof(begin), id, BADGE_VOICE_MAX_BYTES);

        if (length == 0U || !send_line(begin, length)) {
            ESP_LOGW(TAG, "voice_begin 发不出去——链路不在？");
            report.outcome = BADGE_VOICE_SEND_FAILED;
            goto done;
        }
        begun = true;
    }
    badge_adpcm_reset(&enc);

    while (!s_stop && !s_cancel) {
        size_t encoded;

        if (now_ms() - started_ms >= BADGE_VOICE_MAX_MS) {
            ESP_LOGI(TAG, "到上限（%u ms），自动结束", (unsigned)BADGE_VOICE_MAX_MS);
            break;
        }
        if (bsp_audio_read(pcm, sizeof(pcm)) != ESP_OK) {
            ESP_LOGW(TAG, "读麦克风失败（已录 %u 帧）", (unsigned)frames);
            break;
        }
        ++frames;
        // 整段的峰值：见 badge_voice.h 那个字段的注释——「片数和字节数都对」也可能是
        // 整段静音，只有这个数分得开。
        {
            size_t k;

            for (k = 0U; k < BADGE_VOICE_FRAME_SAMPLES; ++k) {
                const int32_t value = (pcm[k] < 0) ? -(int32_t)pcm[k] : (int32_t)pcm[k];

                if (value > (int32_t)peak) {
                    peak = (uint16_t)value;
                }
            }
        }
        encoded = badge_adpcm_encode(&enc, pcm, BADGE_VOICE_FRAME_SAMPLES,
                                     adpcm + filled, sizeof(adpcm) - filled);
        if (encoded == 0U) {
            // 一帧 512 采样编成 256 字节，而一片是 512 字节：只有「片里已经攒了 256
            // 字节以上」才装不下。真发生说明片尺寸与帧尺寸的关系被改乱了，说出来比
            // 静默丢数据好。
            ESP_LOGW(TAG, "编码缓冲装不下（filled=%u）", (unsigned)filled);
            break;
        }
        filled += encoded;
        if (filled >= BADGE_VOICE_RAW_CHUNK) {
            send_part(id, adpcm, BADGE_VOICE_RAW_CHUNK, &sent, &failed, &bytes_sent);
            filled = 0U;
        }
    }
    // 收尾：flush 那半个字节，连同剩下的不满一片的部分作为最后一片发出去。
    filled += badge_adpcm_flush(&enc, adpcm + filled, sizeof(adpcm) - filled);
    if (filled > 0U) {
        send_part(id, adpcm, filled, &sent, &failed, &bytes_sent);
    }
    if (begun) {
        static char end[128];
        // end 报的是**打算发的总片数**（成功的 + 失败的）：主机收到的比它少，就说明
        // 路上丢了——那正是这个字段存在的理由（设计文档 §5）。**取消时报 0**：那让
        // 主机把它已经收到的那些片整段作废（片数对不上），而不是把半截音频送去转写。
        const size_t parts = s_cancel ? 0U : sent + failed;
        const size_t length = badge_voice_end(end, sizeof(end), id, parts);

        if (length == 0U || !send_line(end, length)) {
            ESP_LOGW(TAG, "voice_end 发不出去——主机不会知道这段结束了");
            report.outcome = BADGE_VOICE_SEND_FAILED;
        }
    }
    // 睡回去：平时 codec 不该占电流（与提示音那条同一个约定）。
    if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "codec 没能睡着");
    }
done:
    // 时长按**采到的采样数**算，不按墙钟：调度抖动不该混进「录了多久」。
    report.ms = (uint32_t)((uint64_t)frames * BADGE_VOICE_FRAME_SAMPLES * 1000ULL / BADGE_VOICE_HZ);
    if (s_cancel) {
        // 太短的按住不算「取消」：里面混着双击确定（看账）那两下短按，屏幕上不该因此
        // 冒字（见 badge_voice.h 的 BADGE_VOICE_MIN_MS）。用户自己按上键取消的长按照旧
        // 报「已取消」。
        report.outcome = (report.ms < BADGE_VOICE_MIN_MS) ? BADGE_VOICE_TOO_SHORT
                                                          : BADGE_VOICE_CANCELLED;
    }
    report.parts_sent = sent;
    report.parts_failed = failed;
    report.bytes = (unsigned)bytes_sent;
    report.peak = peak;
    ESP_LOGI(TAG, "录音结束：%u ms，发出 %u 片（失败 %u），%u 字节，峰值 %u，outcome=%d",
             (unsigned)report.ms, sent, failed, (unsigned)report.bytes, (unsigned)peak,
             (int)report.outcome);
    s_report = report;
    s_report_ready = true; // 最后写：读的人靠它判断上面那些数已经填好
    s_recording = false;
    s_busy = false;
    vTaskDelete(NULL);
}

void badge_voice_init(badge_voice_send_fn send, void *context)
{
    s_send = send;
    s_send_context = context;
}

badge_voice_action_t badge_voice_start(void)
{
    if (s_busy) {
        return BADGE_VOICE_ACTION_BUSY; // 上一次还在发（或上一段还在收尾）
    }
    if (badge_sound_busy()) {
        // 提示音此刻占着 codec，而格式互踩是这件事里最容易出错的地方（设计文档 §8：
        // BSP 要求调用方串行化格式与休眠）。那一两声很短，让用户松手再按一次比抢
        // codec 好。
        return BADGE_VOICE_ACTION_BUSY;
    }
    s_stop = false;
    s_cancel = false;
    s_recording = true;
    s_busy = true;
    // 计时从**按下**这一下开始，而不是采集任务真正跑起来那一刻：屏幕上那个倒计时
    // 该跟着手指走，而 badge_voice_finish 判「太短」用的也是同一个起点（差几毫秒的
    // 话，一次刚好卡在门限上的按住会被判成短按）。
    s_record_started_ms = now_ms();
    if (xTaskCreate(voice_task, "badge_voice", VOICE_TASK_STACK, NULL, VOICE_TASK_PRIORITY, NULL) !=
        pdPASS) {
        s_recording = false;
        s_busy = false;
        return BADGE_VOICE_ACTION_NO_AUDIO;
    }
    return BADGE_VOICE_ACTION_STARTED;
}

badge_voice_action_t badge_voice_finish(void)
{
    if (!s_recording) {
        // 松开这一下总会有，包括「录音已经因为到上限自己停了」和「按下被别的屏吃掉了」
        // 那两种情况——没在录就当没发生，调用方不必先问再调。
        return BADGE_VOICE_ACTION_NONE;
    }
    if (now_ms() - s_record_started_ms < BADGE_VOICE_MIN_MS) {
        // 太短的按住：整段作废。已经发出去的 begin（和头几片）由收尾那条 parts=0 的
        // end 一起作废——主机按片数对账，看到的是一段对不上账的音频。
        s_cancel = true;
        return BADGE_VOICE_ACTION_TOO_SHORT;
    }
    s_stop = true;
    return BADGE_VOICE_ACTION_STOPPED;
}

bool badge_voice_take_report(badge_voice_report_t *out)
{
    if (!s_report_ready) {
        return false;
    }
    s_report_ready = false;
    if (out != NULL) {
        *out = s_report;
    }
    return true;
}

bool badge_voice_busy(void)
{
    return s_busy;
}

bool badge_voice_progress(uint32_t *elapsed_ms, uint32_t *limit_ms)
{
    if (!s_recording) {
        return false;
    }
    if (elapsed_ms != NULL) {
        *elapsed_ms = now_ms() - s_record_started_ms;
    }
    if (limit_ms != NULL) {
        *limit_ms = BADGE_VOICE_MAX_MS;
    }
    return true;
}

void badge_voice_cancel(void)
{
    if (s_recording) {
        s_cancel = true; // 采集任务下一轮跳出，然后按「取消」收尾
    }
}
