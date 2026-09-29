// badge_sound —— 详见头文件。
#include "badge_sound.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "bsp_audio.h"

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

static void sound_task(void *argument)
{
    size_t written = 0;

    (void)argument;
    for (;;) {
        uint8_t request = 0;

        if (xQueueReceive(s_requests, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        // codec 平时睡着：这一声的代价是「唤醒 → 写完 → 再睡」，而平时它不占电流。
        if (bsp_audio_wake() != ESP_OK) {
            ESP_LOGW(TAG, "codec 唤醒失败，这一声跳过");
            continue;
        }
        // 留痕：排查「怎么没响」时，这一行是「试过了」与「压根没到」的唯一分界。
        ESP_LOGI(TAG, "响一声（%u 采样）", (unsigned)badge_sound_samples);
        for (written = 0; written < badge_sound_samples; written += SOUND_CHUNK_SAMPLES) {
            size_t chunk = badge_sound_samples - written;

            if (chunk > SOUND_CHUNK_SAMPLES) {
                chunk = SOUND_CHUNK_SAMPLES;
            }
            if (bsp_audio_write(&badge_sound_pcm[written], chunk * sizeof(int16_t)) != ESP_OK) {
                ESP_LOGW(TAG, "播放失败（已写 %u/%u 采样）",
                         (unsigned)written, (unsigned)badge_sound_samples);
                break;
            }
        }
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

    s_requests = xQueueCreate(1, sizeof(uint8_t));
    if (s_requests == NULL || xTaskCreate(sound_task, "badge_sound", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "播放任务未能创建");
    }
}

void badge_sound_play(void)
{
    const uint8_t request = 1;

    if (s_requests == NULL) {
        return;
    }
    // 不等待：队列里已经有一声在路上就够了。
    (void)xQueueSend(s_requests, &request, 0);
}
