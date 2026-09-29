// badge_power —— 「屏幕现在该不该亮」这一件事的全部逻辑。
//
// 它刻意与 ESP-IDF、LVGL 都无关，因为这里有一条不能出错的红线：**屏幕灭着藏一条
// 「有人在等你」**。那条边界（有待答项时不许熄、刚有活动不许熄、按键立刻亮）在主机上
// 就能试出来，不该靠烧板子等 60 秒去看。
//
// 它管的是**屏幕**，不是芯片：熄屏只动背光。这块设备靠一条 USB 串口（以后是 BLE）
// 与主机连着，睡下去就听不到主机递过来的 ask，所以省电的第一层永远是关背光。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// 空闲多久熄屏。
//
// 必须明显大于主机的 10 秒心跳（设备每 10 秒发一次 sync，主机据此回一条 state）——
// 否则「主机在跑但没人待答」时屏幕会被心跳反复点亮又熄灭。
#define BADGE_SCREEN_OFF_MS 60000U

typedef struct {
    bool screen_on;
    uint32_t last_activity_ms;
} badge_power_t;

// badge_power_init 从亮屏开始，空闲计时自 now_ms 起算。
void badge_power_init(badge_power_t *power, uint32_t now_ms);

// badge_power_activity 记一次「有事发生」：按键，或者链路状态变化（连上 / 断开）。
// 它立刻把屏幕点亮并重置空闲计时，返回 true 表示屏幕由灭变亮（调用方据此开背光）。
//
// ⚠ 两件事都别做，否则空闲计时形同虚设：
//   1. 每个 tick 都调它；
//   2. 把「收到任何协议行」也当成活动——主机每 10 秒会回一条心跳式的 state，
//      那样屏幕永远不会灭。待答项由 badge_power_tick 负责，不归这里。
bool badge_power_activity(badge_power_t *power, uint32_t now_ms);

// badge_power_tick 推进空闲计时，返回 true 表示屏幕状态翻转了。
//
// has_pending_ask 为 true 时**永不熄灭**，并且会把灭着的屏幕立刻点亮：屏幕灭着
// 藏一条等待，是本设备唯一不可接受的行为。
//
// 时间用无符号相减比较，所以 now_ms 回绕（约 49.7 天）不会导致误判——这里和
// main.c 判断链路超时是同一套算法。
bool badge_power_tick(badge_power_t *power, uint32_t now_ms, bool has_pending_ask);
