// badge_sound —— 详见头文件。
#include "badge_sound.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp_audio.h"
#include "badge_voice.h" // badge_voice_busy：录音时让路（codec 互斥，见 badge_sound.h）

static const char *TAG = "badge_sound";

// 采样格式：与 demo_audio 用的同一档，也是 BSP 的默认档。
#define SOUND_HZ 16000U
#define SOUND_BITS 16U
#define SOUND_CHANNELS 1U

// 音量比 demo 的 80 低一档：这一声是在人不在旁边时响的，不必喊；而这块小喇叭拉满
// 容易破音。
#define SOUND_VOLUME 70U

// 每次写 512 个采样（32 ms），和 demo 一样：一次把整段 PCM 交给 codec 会让那个调用
// 阻塞整段时长，也把临时缓冲放大到整段音频。
#define SOUND_CHUNK_SAMPLES 512U

static QueueHandle_t s_requests;
// 正在放（含唤醒 codec 的那一段）。录音那条靠它避开：BSP 要求调用方串行化格式与休眠，
// 两边同时碰 ES8311 的结果是录下来一段噪音，而不是一行报错。
static volatile bool s_playing;

// clip_for 返回这一种提示音的 PCM 与长度。
//
// 一处映射，而不是一张「kind → 数组」的表：那张表要么把长度也塞进静态初始化（`const
// size_t` 在 C 里不是常量表达式，塞不进去），要么就得多一份并行的长度表——那种重复
// 迟早会让某一声放错长度。
static const int16_t *clip_for(badge_alert_kind_t kind, size_t *samples)
{
    switch (kind) {
    case BADGE_ALERT_DONE:
        *samples = badge_sound_done_samples;
        return badge_sound_done_pcm;
    case BADGE_ALERT_ASK:
    default:
        *samples = badge_sound_ask_samples;
        return badge_sound_ask_pcm;
    }
}

static void sound_task(void *argument)
{
    (void)argument;
    for (;;) {
        badge_alert_kind_t kind = BADGE_ALERT_NONE;
        size_t samples = 0;
        const int16_t *pcm;
        size_t written;

        if (xQueueReceive(s_requests, &kind, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (kind == BADGE_ALERT_NONE) {
            continue;
        }
        // 录音在用 codec：这一声丢掉。不为它排队——录音是几秒的主动操作，而「有事等你」
        // 延后几秒就没有意义了；更糟的是两边同时碰 ES8311 会让录下来的东西变成噪音。
        if (badge_voice_busy()) {
            ESP_LOGI(TAG, "正在录音，这一声跳过");
            continue;
        }
        pcm = clip_for(kind, &samples);
        // codec 平时睡着：这一声的代价是「唤醒 → 写完 → 再睡」，而平时它不占电流。
        if (bsp_audio_wake() != ESP_OK) {
            ESP_LOGW(TAG, "codec 唤醒失败，这一声跳过");
            continue;
        }
        // 留痕：排查「怎么没响」时，这一行是「试过了」与「压根没到」的唯一分界。
        ESP_LOGI(TAG, "响一声（%s，%u 采样）", kind == BADGE_ALERT_DONE ? "done" : "ask",
                 (unsigned)samples);
        s_playing = true;
        for (written = 0; written < samples; written += SOUND_CHUNK_SAMPLES) {
            size_t chunk = samples - written;

            if (chunk > SOUND_CHUNK_SAMPLES) {
                chunk = SOUND_CHUNK_SAMPLES;
            }
            if (bsp_audio_write(&pcm[written], chunk * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGW(TAG, "播放失败（已写 %u/%u 采样）", (unsigned)written, (unsigned)samples);
                break;
            }
        }
        // 先清标志再睡：录音那条等在 s_playing 上，而睡下去之后 codec 就归它了。
        s_playing = false;
        if (bsp_audio_sleep() != ESP_OK) {
            ESP_LOGW(TAG, "codec 没能睡着");
        }
    }
}

void badge_sound_init(void)
{
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败——这台设备不会响，别的功能照旧");
        return;
    }
    if (bsp_audio_set_format(SOUND_HZ, SOUND_BITS, SOUND_CHANNELS) != ESP_OK) {
        ESP_LOGW(TAG, "音频格式设置失败");
        return;
    }
    bsp_audio_set_volume(SOUND_VOLUME);
    // 送去睡眠要放在 set_format 之后：它会释放 codec 对象（见 bsp_audio.h 的约定），
    // 唤醒时再重建并恢复格式与音量。
    if (bsp_audio_sleep() != ESP_OK) {
        ESP_LOGW(TAG, "codec 没能睡着（能响，只是平时多耗一点）");
    }

    s_requests = xQueueCreate(1, sizeof(badge_alert_kind_t));
    if (s_requests == NULL || xTaskCreate(sound_task, "badge_sound", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "播放任务未能创建");
    }
}

void badge_sound_play(badge_alert_kind_t kind)
{
    if (s_requests == NULL || kind == BADGE_ALERT_NONE) {
        return;
    }
    // 覆盖队列里那一格：连着两声的意义是零，而以最后那种为准（和状态机同一条约定）。
    if (xQueueOverwrite(s_requests, &kind) != pdTRUE) {
        ESP_LOGW(TAG, "提示音请求未能入队");
    }
}

bool badge_sound_busy(void)
{
    return s_playing;
}
