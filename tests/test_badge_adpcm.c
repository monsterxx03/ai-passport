// badge_adpcm 的单元测试。
//
// 这里钉的不是「编出来的字节好不好听」——那听不出来。钉的是**两端一致**：编码器的初值、
// 打包次序、步长表，任何一处与主机侧的解码器不一致，解出来都是噪音，而设备这边看起来
// 一切正常（发得出去、长度也对）。所以第一组断言直接写死在
// tools/stt-probe/adpcm_roundtrip.py 上跑出来的字节上。
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "badge_adpcm.h"

// 覆盖两头与大跳变的样本：0、整数斜坡、±32767/-32768 的极值、以及几千量级的来回。
static const int16_t kVectors[] = {
    0, 1000, -1000, 4000, -4000, 12000, -12000, 32767, -32768,
    0, 1, -1, 100, -100, 5000, 6000, 7000, 8000, 3000, -3000,
    20000, 21000, 22000, -20000, -21000, 16384, -16384, 12345, -23456, 777, -777, 42,
};
// adpcm_roundtrip.py 对同一串样本的输出（改编码器之前先重跑那份 Python）。
static const uint8_t kVectorsEncoded[] = {
    0xF8, 0xF7, 0xF7, 0xF7, 0xA7, 0x80, 0x08, 0x9F,
    0x89, 0x45, 0xBF, 0x78, 0xD4, 0xB3, 0xB5, 0x80,
};

static void test_matches_the_python_reference(void)
{
    badge_adpcm_enc_t enc;
    uint8_t out[sizeof(kVectors) / 2U + 1U];

    badge_adpcm_reset(&enc);
    const size_t n = badge_adpcm_encode(&enc, kVectors,
                                        sizeof(kVectors) / sizeof(kVectors[0]),
                                        out, sizeof(out));
    assert(n == sizeof(kVectorsEncoded));
    assert(memcmp(out, kVectorsEncoded, sizeof(kVectorsEncoded)) == 0);
    // 编码器的末态也要对上：主机接下来解出来的样本由它决定。
    assert(enc.index == 85U);
    assert(enc.predictor == 2043);
}

// 麦克风的采样是一帧一帧来的，块大小由调用方定。**分两次喂必须和一次喂一模一样**：
// 半个字节攒在编码器里（pending），不这么做的话每个接缝都会错半字节。
static void test_chunking_does_not_shift_the_stream(void)
{
    badge_adpcm_enc_t enc;
    const size_t count = sizeof(kVectors) / sizeof(kVectors[0]);
    uint8_t one[sizeof(kVectors) / 2U + 1U];
    uint8_t split[sizeof(kVectors) / 2U + 1U];
    size_t written = 0U;
    size_t n;
    size_t i;

    badge_adpcm_reset(&enc);
    n = badge_adpcm_encode(&enc, kVectors, count, one, sizeof(one));
    assert(n == sizeof(kVectorsEncoded));

    // 一次一个样本——最极端的切法，每个字节都跨两次调用。
    badge_adpcm_reset(&enc);
    for (i = 0U; i < count; ++i) {
        written += badge_adpcm_encode(&enc, &kVectors[i], 1U, split + written,
                                      sizeof(split) - written);
    }
    assert(written == n);
    assert(memcmp(one, split, n) == 0);

    // 奇数个样本一组（3 个、3 个……）——采样数与字节数的对不齐最容易被忽略。
    badge_adpcm_reset(&enc);
    written = 0U;
    for (i = 0U; i < count; i += 3U) {
        const size_t take = (count - i < 3U) ? (count - i) : 3U;

        written += badge_adpcm_encode(&enc, &kVectors[i], take, split + written,
                                      sizeof(split) - written);
    }
    assert(written == n);
    assert(memcmp(one, split, n) == 0);
}

// 录完最后半个字节要补出来（高 4 位补 0），否则最后一个采样就丢了。
static void test_flush_completes_the_last_byte(void)
{
    badge_adpcm_enc_t enc;
    uint8_t out[8];
    uint8_t tail = 0xEEU;
    size_t n;

    badge_adpcm_reset(&enc);
    // 5 个采样 = 2 个整字节 + 一个 pending 的 code（Python 那边也是这个数：0xF8 0xF7）。
    n = badge_adpcm_encode(&enc, kVectors, 5U, out, sizeof(out));
    assert(n == 2U);
    assert(out[0] == 0xF8U && out[1] == 0xF7U);
    assert(badge_adpcm_flush(&enc, &tail, 1U) == 1U);
    assert(tail == 7U); // 剩的那个 code

    // 已经补过了就没有第二份：重复 flush 不该再吐一个字节。
    assert(badge_adpcm_flush(&enc, &tail, 1U) == 0U);
}

// 缓冲不够时一个字节都不写、状态也不动——调用方按「这条没成」处理是对的。
static void test_short_buffer_writes_nothing(void)
{
    badge_adpcm_enc_t enc;
    badge_adpcm_enc_t before;
    uint8_t out[4] = {0xAA, 0xAA, 0xAA, 0xAA};

    badge_adpcm_reset(&enc);
    before = enc;
    assert(badge_adpcm_encode(&enc, kVectors, 32U, out, 8U) == 0U); // 需要 16
    assert(out[0] == 0xAAU);
    assert(memcmp(&enc, &before, sizeof(enc)) == 0);

    // 正好够就可以。
    assert(badge_adpcm_encode(&enc, kVectors, 32U, out, 16U) == 16U);
}

// 极值不该让它越界（预测值会一路顶到 ±32767/±32768 的边界上）。
static void test_extremes_stay_in_range(void)
{
    badge_adpcm_enc_t enc;
    int16_t loud[64];
    uint8_t out[40];
    size_t i;

    for (i = 0U; i < 64U; ++i) {
        loud[i] = (i % 2U == 0U) ? 32767 : -32768;
    }
    badge_adpcm_reset(&enc);
    assert(badge_adpcm_encode(&enc, loud, 64U, out, sizeof(out)) == 32U);
    assert(enc.predictor >= -32768 && enc.predictor <= 32767);
    assert(enc.index <= 88U);

    // 全 0 是合法的安静：编出来全是 0 附近的码，而且长度准确。
    memset(loud, 0, sizeof(loud));
    badge_adpcm_reset(&enc);
    assert(badge_adpcm_encode(&enc, loud, 64U, out, sizeof(out)) == 32U);
}

// 空输入是合法的（麦克风这一帧没数据），不该动状态也不该写字节。
static void test_empty_input(void)
{
    badge_adpcm_enc_t enc;
    uint8_t out[4];

    badge_adpcm_reset(&enc);
    assert(badge_adpcm_encode(&enc, kVectors, 0U, out, sizeof(out)) == 0U);
    assert(!enc.has_pending);
    assert(badge_adpcm_flush(&enc, out, sizeof(out)) == 0U);
}

int main(void)
{
    test_matches_the_python_reference();
    test_chunking_does_not_shift_the_stream();
    test_flush_completes_the_last_byte();
    test_short_buffer_writes_nothing();
    test_extremes_stay_in_range();
    test_empty_input();
    printf("test_badge_adpcm: OK\n");
    return 0;
}
