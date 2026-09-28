// badge_link —— 与 tachi 之间的那条字节流（USB-Serial-JTAG）。
//
// 这一层的全部难点来自一个事实：**设备的控制台日志和协议走同一条通道**。日志是
// 别人的（ESP-IDF、LVGL、各个驱动都会往里写），协议是我们的，两者在主机侧也混在
// 一起，所以只能靠 `@@` 前缀区分。本文件负责按行分帧、只把带前缀的行交出去。
//
// 另一个来自这块板的坑：不显式安装 USB-Serial-JTAG 驱动就开始读，会解引用空的驱动
// 对象并反复重启——而背光已经点亮，用户看到的是「屏幕一直在闪」，不是一行报错。
// 同类问题（读错误时忙等会饿死空闲任务、触发看门狗）也在归档里有过记录
// （ai-passport docs/reference/y2lin/serial-screenshot-protocol）。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// badge_link_line_cb_t 收到一行。line 不含 `@@` 前缀与行尾，也**不是** NUL 结尾——
// 按 length 读。
typedef void (*badge_link_line_cb_t)(const char *line, size_t length, void *context);

// badge_link_start 安装驱动并起读取任务。重复调用是空操作。
esp_err_t badge_link_start(badge_link_line_cb_t callback, void *context);
void badge_link_stop(void);

// badge_link_send 发一条协议行：内部补上 `@@` 前缀和换行。
//
// 返回 false 表示这一行没发全（缓冲区满、主机没在读），调用方应当把它当作一次
// 失败——半行消息在主机那是一行永远解析不了的东西，比不发更坏。
bool badge_link_send(const char *data, size_t length);
