#include "badge_adpcm.h"

// 步长表与索引调整表：IMA ADPCM 的全部"知识"都在这两张表里。数值与
// tools/stt-probe/adpcm_roundtrip.py 的 STEP / INDEX 一一对应，改动前先读那个文件顶上
// 那段——两边不一致的后果是主机解出噪音，而不是某个能看出来的错。
static const int32_t ADPCM_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};
static const int8_t ADPCM_INDEX[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

void badge_adpcm_reset(badge_adpcm_enc_t *enc)
{
    if (enc == NULL) {
        return;
    }
    enc->index = 0U;
    enc->predictor = 0;
    enc->pending = 0U;
    enc->has_pending = false;
}

// encode_sample 编一个采样，推进编码器的状态。顺序与 Python 的 enc() 完全一致：
// 先按差值定 4 个 bit，再按同一个差值修预测值，最后按 code 调步长索引。
static uint8_t encode_sample(badge_adpcm_enc_t *enc, int16_t sample)
{
    const int32_t step = ADPCM_STEP[enc->index];
    int32_t diff = (int32_t)sample - enc->predictor;
    int32_t vpdiff;
    uint8_t code = 0U;
    int32_t index;

    if (diff >= 0) {
        code = 8U; // bit3 = 方向
    } else {
        diff = -diff;
    }
    vpdiff = step >> 3;
    if (diff >= step) {
        code |= 4U;
        diff -= step;
        vpdiff += step;
    }
    if (diff >= (step >> 1)) {
        code |= 2U;
        diff -= step >> 1;
        vpdiff += step >> 1;
    }
    if (diff >= (step >> 2)) {
        code |= 1U;
        vpdiff += step >> 2;
    }

    enc->predictor += (code & 8U) ? vpdiff : -vpdiff;
    if (enc->predictor > 32767) {
        enc->predictor = 32767;
    }
    if (enc->predictor < -32768) {
        enc->predictor = -32768;
    }
    index = (int32_t)enc->index + ADPCM_INDEX[code];
    if (index < 0) {
        index = 0;
    }
    if (index > 88) {
        index = 88;
    }
    enc->index = (uint8_t)index;
    return code;
}

size_t badge_adpcm_encode(badge_adpcm_enc_t *enc, const int16_t *samples, size_t count,
                          uint8_t *out, size_t cap)
{
    size_t written = 0U;
    size_t need;
    size_t i;

    if (enc == NULL || (samples == NULL && count > 0U) || out == NULL) {
        return 0U;
    }
    // 先算够不够，再动手：装不下时一个字节都不写、状态也不动（调用方按 0 处理）。
    // 半个字节已经攒在 pending 里时，第一个新采样会把它凑成一个整字节。
    need = (count + (enc->has_pending ? 1U : 0U)) / 2U;
    if (need > cap) {
        return 0U;
    }
    for (i = 0U; i < count; ++i) {
        const uint8_t code = encode_sample(enc, samples[i]);

        if (!enc->has_pending) {
            enc->pending = code;
            enc->has_pending = true;
            continue;
        }
        out[written++] = (uint8_t)(enc->pending | (uint8_t)(code << 4));
        enc->pending = 0U;
        enc->has_pending = false;
    }
    return written;
}

size_t badge_adpcm_flush(badge_adpcm_enc_t *enc, uint8_t *out, size_t cap)
{
    if (enc == NULL || out == NULL || !enc->has_pending || cap < 1U) {
        return 0U;
    }
    // 高 4 位补 0——和 Python 的 `packed.append(pend)` 一致（那里也没有补一个采样）。
    out[0] = enc->pending;
    enc->pending = 0U;
    enc->has_pending = false;
    return 1U;
}
