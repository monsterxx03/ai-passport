#!/usr/bin/env python3
"""把 16 kHz 单声道 PCM 过一遍标准 IMA ADPCM 4:1（编码→解码），
模拟设备→主机链路里的有损压缩，用来量它会不会打掉识别率。"""
import sys
import wave
import struct

STEP = [7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
        73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,
        408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,
        1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,
        7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
        22385,24623,27086,29794,32767]
INDEX = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8]


def enc(sample, idx, pred):
    step = STEP[idx]
    diff = sample - pred
    code = 8 if diff >= 0 else 0
    if diff < 0:
        diff = -diff
    vpdiff = step >> 3
    if diff >= step:
        code |= 4; diff -= step; vpdiff += step
    if diff >= step >> 1:
        code |= 2; diff -= step >> 1; vpdiff += step >> 1
    if diff >= step >> 2:
        code |= 1; vpdiff += step >> 2
    pred = pred + vpdiff if code & 8 else pred - vpdiff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + INDEX[code]))
    return code, idx, pred


def dec(code, idx, pred):
    step = STEP[idx]
    vpdiff = step >> 3
    if code & 4: vpdiff += step
    if code & 2: vpdiff += step >> 1
    if code & 1: vpdiff += step >> 2
    pred = pred + vpdiff if code & 8 else pred - vpdiff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + INDEX[code & 7]))
    return pred, idx


def roundtrip(src, dst, nibbles_per_byte=True):
    with wave.open(src, 'rb') as w:
        assert w.getnchannels() == 1 and w.getsampwidth() == 2, "只处理单声道 16-bit"
        rate = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    samples = struct.unpack('<%dh' % n, raw)

    # 编码：每两个 4-bit 码打进一个字节（低位在前），和设备的打包方式一致。
    idx, pred = 0, 0
    packed = bytearray()
    pend = None
    for s in samples:
        code, idx, pred = enc(s, idx, pred)
        if pend is None:
            pend = code
        else:
            packed.append(pend | (code << 4))
            pend = None
    if pend is not None:
        packed.append(pend)
    codes = len(packed) * 2

    # 解码
    idx, pred = 0, 0
    out = []
    for b in packed:
        for code in (b & 0xF, (b >> 4) & 0xF):
            if len(out) >= n:
                break
            v, idx = dec(code, idx, pred)
            pred = v
            out.append(v)

    with wave.open(dst, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(out), *out))
    return rate, n, len(packed), codes


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    rate, n, bytes_, codes = roundtrip(src, dst)
    print(f"{rate} Hz, {n} 采样 ({n*2} 字节 PCM) → {bytes_} 字节 ADPCM "
          f"({bytes_*8/n:.2f} bit/采样)")
