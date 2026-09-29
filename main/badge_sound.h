// badge_sound —— 提示音：主机说「这条等待值得响一声」时，放一声短铃。
//
// 触发来自主机（tachi 的 registerAsk：不在前台、或者不是当前活跃会话），设备这边只负责
// 响。「什么时候该响」是主机的知识——它才知道自己在前台没有、用户正在看哪个会话；这块
// 屏上没有别的信息能推出这两个事实。
#pragma once

#include <stddef.h>
#include <stdint.h>

// 生成的 PCM（tools/gen_badge_sound.py）：16 kHz / 单声道 / 16-bit。
extern const int16_t badge_sound_pcm[];
extern const size_t badge_sound_samples;

// badge_sound_init 初始化 codec 并起播放任务（app_main 里调一次）。
//
// 失败只记一行日志：这块板子上音频是可选的，它起不来不该拦住别的（与 BLE 那条同一条
// 约定）。初始化完立刻把 codec 送去睡眠——只在真的响的时候唤醒，平时不占电流。
void badge_sound_init(void);

// badge_sound_play 请求响一声。从任何任务调都行，不阻塞：队列只有一格，连着来两声只
// 会响一声（提示音不是日志，重复的意义是零）。
void badge_sound_play(void);
