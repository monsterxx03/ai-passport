#include "badge_link.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
// usb_serial_jtag_vfs_use_driver 住在另一个头文件里（5.5 把驱动拆成了
// esp_driver_usb_serial_jtag 组件），不 include 它就会在这里编译不过。
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "badge_link";

// 协议行前缀。它同时是主机侧的过滤条件（desktop/link/link.go 只认带它的行）。
#define LINE_PREFIX "@@"
#define LINE_PREFIX_LENGTH (sizeof(LINE_PREFIX) - 1U)

// 收发缓冲。写入必须远小于 tx 缓冲：usb_serial_jtag_write_bytes 落在环形缓冲上，
// 一次提交超过容量的整块会直接失败（一个字节都发不出去）。
#define BADGE_LINK_TX_CHUNK 512U
#define BADGE_LINK_TX_BUFFER 1024U
#define BADGE_LINK_RX_BUFFER 1024U

// 一行消息的上限。提问的题面可能很长，但设备侧的回传上限（BADGE_QUESTION_MAX）
// 比它小，所以超出这个长度的行只可能是坏数据。
#define BADGE_LINK_LINE_MAX 4096U

#define BADGE_LINK_READ_CHUNK 128U
#define BADGE_LINK_TASK_STACK 4096U
// 低于 LVGL（4）：这条链路是配角，读任务的调度不该跟界面抢。
#define BADGE_LINK_TASK_PRIORITY 3U

static badge_link_line_cb_t s_callback;
static void *s_context;
static TaskHandle_t s_task;
static bool s_installed;

static char s_line[BADGE_LINK_LINE_MAX];
static size_t s_line_length;
static bool s_overflow;

static void deliver(void)
{
    size_t length = s_line_length;

    s_line_length = 0;
    if (s_overflow) {
        // 一句都不说是最坏的处理方式：上层只会看到「设备没反应」。
        ESP_LOGW(TAG, "line dropped: exceeds %u bytes", (unsigned)BADGE_LINK_LINE_MAX);
        s_overflow = false;
        return;
    }
    if (length > 0U && s_line[length - 1U] == '\r') {
        --length; // 终端工具可能补一个回车
    }
    if (length < LINE_PREFIX_LENGTH ||
        strncmp(s_line, LINE_PREFIX, LINE_PREFIX_LENGTH) != 0) {
        return; // 控制台日志：不是给我们的
    }
    if (s_callback != NULL) {
        s_callback(s_line + LINE_PREFIX_LENGTH, length - LINE_PREFIX_LENGTH, s_context);
    }
}

static void feed(const char *data, size_t length)
{
    size_t i;

    for (i = 0; i < length; ++i) {
        char c = data[i];

        if (c == '\n') {
            deliver();
            continue;
        }
        if (s_line_length < sizeof(s_line)) {
            s_line[s_line_length++] = c;
        } else {
            // 一行超过了缓冲：丢掉它，但继续吃掉后续字节直到换行——不然后半个
            // 长行会被当成下一条消息的开头。
            s_overflow = true;
            s_line_length = 0;
        }
    }
}

static void badge_link_task(void *argument)
{
    char chunk[BADGE_LINK_READ_CHUNK];

    (void)argument;
    for (;;) {
        int received = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(100));

        if (received > 0) {
            feed(chunk, (size_t)received);
            continue;
        }
        if (received < 0) {
            // 读失败必须退避：一个紧凑的重试循环会打满这个任务、饿死空闲任务并触发
            // 看门狗——在本板上表现为设备反复重启，而日志一条也看不到。
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        // received == 0：只是超时，回到读等待，不需要额外的延时。
    }
}

esp_err_t badge_link_start(badge_link_line_cb_t callback, void *context)
{
    usb_serial_jtag_driver_config_t config = {
        .rx_buffer_size = BADGE_LINK_RX_BUFFER,
        .tx_buffer_size = BADGE_LINK_TX_BUFFER,
    };

    s_callback = callback;
    s_context = context;
    if (s_task != NULL) {
        return ESP_OK; // 已经在跑
    }
    if (!s_installed && !usb_serial_jtag_is_driver_installed()) {
        esp_err_t err = usb_serial_jtag_driver_install(&config);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "driver install failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    s_installed = true;
    // 让控制台输出也走驱动通道。这一步与「驱动是否本来就装好了」无关，所以放在
    // 条件之外：不做它，日志与协议会经由两条不同的路径出去，而只有这条是我们
    // 能看见的（重复调用是安全的）。
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(badge_link_task, "badge_link", BADGE_LINK_TASK_STACK, NULL,
                    BADGE_LINK_TASK_PRIORITY, &s_task) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "read task creation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void badge_link_stop(void)
{
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_line_length = 0;
    s_overflow = false;
}

// write_all 分块把整段写完。返回 false 时可能已经写出去了前半截——半个消息在主机
// 那边是一行解析不了的字节，会被当作坏数据丢掉，这比凭猜补齐要安全。
static bool write_all(const char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        size_t chunk = length - offset;
        int written;

        if (chunk > BADGE_LINK_TX_CHUNK) {
            chunk = BADGE_LINK_TX_CHUNK;
        }
        written = usb_serial_jtag_write_bytes(data + offset, chunk, pdMS_TO_TICKS(200));
        if (written <= 0) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

bool badge_link_send(const char *data, size_t length)
{
    if (s_task == NULL || data == NULL) {
        return false;
    }
    if (length > BADGE_LINK_LINE_MAX) {
        return false;
    }
    return write_all(LINE_PREFIX, LINE_PREFIX_LENGTH) && write_all(data, length) &&
           write_all("\n", 1U);
}
