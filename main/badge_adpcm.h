// badge_adpcm —— IMA ADPCM（4:1）编码器：16 kHz 单声道 PCM → 8 KB/s 的字节流。
//
// 为什么是这一套：它是设计文档 §4 量出来的格式（ADPCM 4:1 在识别率上不付代价），而这条
// 链路那头的解码器**已经有一份可执行的参照**——本仓库 tools/stt-probe/adpcm_roundtrip.py
// 里那份 Python（当初就是用它模拟链路、量识别率的）。
//
// 两边必须**逐字节一致**：初始状态、打包次序、步长表，错一处主机解出来就是噪音，而设备
// 这边一切正常（发得出去、长度也对）。所以这个文件的常量与顺序都对着那份 Python 写，
// tests/test_badge_adpcm.c 里还钉着它跑出来的字节。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 编码器状态。**初值必须是 index=0 / predictor=0**（与 Python 那份相同）：主机的解码器
// 从同样的状态起步，不一致的话第一片就解成噪音。
typedef struct {
    uint8_t index;
    int32_t predictor;
    // 打包到一半的字节。ADPCM 每采样 4 bit，而链路以字节为单位：**低位先装**
    // （Python 的 `pend | (code << 4)`），也就是时间上更早的采样在低 4 位。
    uint8_t pending;
    bool has_pending;
} badge_adpcm_enc_t;

void badge_adpcm_reset(badge_adpcm_enc_t *enc);

// badge_adpcm_encode 把 count 个 int16 采样编成 ADPCM，写进 out（上限 cap 字节）。
// 返回写出的**完整字节数**；cap 不够时返回 0，且一个字节都不写（状态也不动）。
//
// 采样数为奇数时最后一个采样会留在 pending 里等下一次调用——所以调用方可以按任意块大小
// 喂（麦克风一帧一帧地来），字节流不会在接缝处错位。
size_t badge_adpcm_encode(badge_adpcm_enc_t *enc, const int16_t *samples, size_t count,
                          uint8_t *out, size_t cap);

// badge_adpcm_flush 把 pending 那半个字节补出来（高 4 位补 0）——录音结束时调它，否则
// 最后一个采样会丢。返回写出的字节数（0 或 1）。
size_t badge_adpcm_flush(badge_adpcm_enc_t *enc, uint8_t *out, size_t cap);
