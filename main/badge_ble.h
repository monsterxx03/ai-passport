// badge_ble —— 设备侧的 BLE 链路（GATT peripheral），与 badge_link.c 并列的第二条传输。
//
// 主机（tachi）一次只用一条传输（见那份设计文档的「单链路」一节），但设备侧两条都在线：
// 状态机不知道自己在跟谁说话——两条路各自把「一行」交给同一个回调，各自接受同一个
// 「发一行」。哪个方向用哪条路由 main.c 决定。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// 一行（已剥掉 @@ 前缀）的回调。跑在 NimBLE 的任务上：只能入队，不能阻塞、不能碰 LVGL
// ——与 badge_link 的回调是同一条约定。
typedef void (*badge_ble_line_cb_t)(const char *line, size_t length, void *context);

// 链路状态。界面靠它区分「没配对 / 正在配对 / 配过但没连上 / 连上且安全」——这四种
// 情况下用户要做的事完全不同（去配对 / 输码 / 打开 tachi / 什么都不用做）。
typedef enum {
    BADGE_BLE_UNPAIRED = 0, // 没有 bond：在广播，等电脑来配
    BADGE_BLE_PAIRING,      // 正在配对：屏幕上必须显示那 6 位码
    BADGE_BLE_PAIRED,       // 有 bond，但没连上
    BADGE_BLE_SECURED,      // 连上了，而且链路已认证加密（只有这时才允许说协议）
} badge_ble_state_t;

// badge_ble_start 起链路：初始化协议栈、注册 GATT、开始广播。可重复调用（更新回调）。
esp_err_t badge_ble_start(badge_ble_line_cb_t callback, void *context);

// badge_ble_send 发一整行（正文，不含前缀与换行）：**成帧由传输负责**——补上 @@ 与
// 换行，再按协商到的 MTU 切开逐个 notify。链路没到「已认证加密」或对方没订阅时返回
// false —— 一个字都不发（并把三个条件打出来：这个返回是静默的）。
//
// 成帧不能省：GATT 只保证「来了一段」，而收方（tachi 的 link 层）是**字节流**——它按
// 换行切行、只认 @@ 开头的行。少一个字节，每一行都会被当控制台日志丢掉，而发方这边
// 一切正常（真机上「连上了、却什么也送不回来」就是这么来的）。
bool badge_ble_send(const char *data, size_t length);

badge_ble_state_t badge_ble_state(void);

// 正在配对时屏幕要显示的 6 位码；不在配对态返回空串。
const char *badge_ble_passkey(void);

// 忘记配对的电脑：删掉所有 bond、断开当前连接、重新广播。电脑那边还留着它的那份，
// 所以之后它会「连上即断」——界面必须同时引导用户去系统设置 Forget 那块设备。
void badge_ble_forget(void);

// badge_ble_drop 主动断开当前连接（没有连接时是个空操作）。
//
// 存在的理由：链路层并不知道对面已经没了。上位机崩溃或被 kill 时这里收不到断开事件，
// 而设备卡在「已连接」状态就**永远不会重新广播**——主机侧的症状是「设备不见了」，
// 屏幕上却一切正常。所以要由沉默来推动这一刀。
void badge_ble_drop(void);

// badge_ble_idle_ms 返回「距离最后一次收到主机数据过去了多久」，没连接时返回 0。
//
// 判据刻意不看协议层的 connected：那个标志是「收到过一条完整协议行」才置位的，而
// 「连上、订阅了、然后一个字节都没发」恰恰是最容易把设备卡住的一种处境。
uint32_t badge_ble_idle_ms(void);
