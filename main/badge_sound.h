// badge_sound —— 提示音：主机说「响一声」时，按它给的含义放对应的那一段。
//
// 触发与含义都来自主机（tachi）：它才知道自己在不在前台、用户正在看哪个会话，而这块屏上
// 一条信息都没有。设备只负责放——两种含义放**两段不同的音频**，人不用看屏幕就能分开。
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "badge_proto.h" // badge_alert_kind_t：响哪一种，由协议里的 alert 消息决定

// 生成的两段 PCM（tools/gen_badge_sound.py）：16 kHz / 单声道 / 16-bit。
extern const int16_t badge_sound_ask_pcm[];  // 有事等你（权限确认 / 提问）
extern const size_t badge_sound_ask_samples;
extern const int16_t badge_sound_done_pcm[]; // 一个回合跑完了
extern const size_t badge_sound_done_samples;

// badge_sound_init 初始化 codec 并起播放任务（app_main 里调一次）。
//
// 失败只记一行日志：这块板子上音频是可选的，它起不来不该拦住别的（与 BLE 那条同一条
// 约定）。初始化完立刻把 codec 送去睡眠——只在真的响的时候唤醒，平时不占电流。
void badge_sound_init(void);

// badge_sound_play 请求放一段提示音。从任何任务调都行，不阻塞：队列只有一格，连着来
// 两声只会响一声，而以**最后**那一格为准（状态机也是这么处理的，见 badge_state）。
void badge_sound_play(badge_alert_kind_t kind);
