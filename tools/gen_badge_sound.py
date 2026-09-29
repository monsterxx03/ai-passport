# /// script
# dependencies = ["numpy"]
# ///
"""把一段音频转成设备侧能直接播的 PCM（main/badge_sound_16k.c）。

    uv run tools/gen_badge_sound.py              # 合成默认的提示音（一声短铃）
    uv run tools/gen_badge_sound.py my.wav       # 换成自己的素材

只吃 **WAV**：mp3/m4a 的解码要么拖进 ffmpeg 这个外部依赖，要么塞一个解码器进固件，而换
素材一年也就一两次——先用 macOS 自带的 afconvert 转一下就行：

    afconvert -f WAVE -d LEI16@16000 -c 1 my.m4a my.wav

输出是 16 kHz / 单声道 / 16-bit：BSP 的 bsp_audio_set_format(16000, 16, 1) 就是 demo 用的
那一档，人声与短铃在这个采样率上都够（采样率再高，这块小喇叭也放不出来）。

⚠ 素材的授权：脚本产出的 `.c` 是**入库**的（克隆下来直接就能构建）。所以换素材时要么用
自己录的/CC0 的，要么把产物留在本地别提交——用别人的录音（影视角色的语音之类）提交进
仓库，等于把它一起分发了。
"""
import os
import pathlib
import subprocess
import sys
import wave

import numpy as np

HZ = 16000
OUT = str(pathlib.Path(__file__).resolve().parents[1] / "main" / "badge_sound_16k.c")

# 默认提示音：一声短铃。E6 加一点二次谐波（纯正弦听着太单薄），指数衰减到人耳听不出，
# 总长 0.25 s —— 短是刻意的：提示音要说的是「有事等你」，不是把一首曲子放完。
TONE_HZ = 1318.5
TONE_SECONDS = 0.25
TONE_DECAY = 0.06
TONE_PEAK = 0.55  # 满幅 1.0；留出余量，免得和系统音量叠加后削顶
FADE_SECONDS = 0.004  # 两端各淡入淡出，避免起止那一下「啪」


def synth_tone() -> np.ndarray:
    n = int(HZ * TONE_SECONDS)
    t = np.arange(n) / HZ
    wave_ = np.sin(2 * np.pi * TONE_HZ * t) + 0.25 * np.sin(2 * np.pi * 2 * TONE_HZ * t)
    env = np.exp(-t / TONE_DECAY)
    fade = max(1, int(HZ * FADE_SECONDS))
    ramp = np.linspace(0.0, 1.0, fade)
    env[:fade] *= ramp
    env[-fade:] *= ramp[::-1]
    return wave_ * env / np.max(np.abs(wave_ * env)) * TONE_PEAK


def read_wav(path: str) -> np.ndarray:
    with wave.open(path, "rb") as f:
        ch, width, rate, frames = f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getnframes()
        raw = f.readframes(frames)
    if width != 2:
        raise SystemExit(f"{path}: 只吃 16-bit WAV（这份是 {width * 8}-bit），先转一下：\n"
                         f"  afconvert -f WAVE -d LEI16@16000 -c 1 {path} out.wav")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    if ch > 1:
        data = data.reshape(-1, ch).mean(axis=1)  # 混成单声道
    if rate != HZ:
        # 线性重采样就够了：这是提示音，不是母带。
        dst = np.arange(int(len(data) * HZ / rate)) * (rate / HZ)
        data = np.interp(dst, np.arange(len(data)), data)
    return data


def trim_and_shape(data: np.ndarray) -> np.ndarray:
    # 掐掉首尾的静音：素材前面那 0.3 秒空白会让提示音「延迟出现」，而这条链路上
    # 延迟正是它要解决的问题。阈值取满幅的 1%。
    loud = np.where(np.abs(data) > 0.01 * np.max(np.abs(data)))[0]
    if len(loud):
        data = data[loud[0]:loud[-1] + 1]
    fade = max(1, int(HZ * FADE_SECONDS))
    ramp = np.linspace(0.0, 1.0, min(fade, len(data) // 2))
    data = data.copy()
    data[:len(ramp)] *= ramp
    data[-len(ramp):] *= ramp[::-1]
    return data / np.max(np.abs(data)) * TONE_PEAK


def emit(samples: np.ndarray) -> None:
    pcm = np.clip(samples * 32767.0, -32768, 32767).astype("<i2")
    rows = [", ".join(str(int(v)) for v in pcm[i:i + 16]) for i in range(0, len(pcm), 16)]
    body = ",\n    ".join(rows)
    with open(OUT, "w") as f:
        f.write(f"""// 生成的提示音（tools/gen_badge_sound.py），别手改。
//
// {len(pcm)} 个采样 = {len(pcm) / HZ:.2f} 秒 @ {HZ} Hz / 单声道 / 16-bit。
// 换素材：uv run tools/gen_badge_sound.py <自己的 wav>，然后重新构建。
#include "badge_sound.h"

const int16_t badge_sound_pcm[] = {{
    {body}
}};

const size_t badge_sound_samples = {len(pcm)};
""")
    print(f"已生成 {OUT}（{len(pcm)} 采样，{len(pcm) / HZ:.2f} 秒，"
          f"{os.path.getsize(OUT) // 1024} KB 源码）")


def main() -> None:
    if len(sys.argv) > 1:
        src = sys.argv[1]
        if not os.path.exists(src):
            raise SystemExit(f"找不到输入文件：{src}")
        if not src.lower().endswith(".wav"):
            raise SystemExit("只吃 WAV，先转一下：\n"
                             f"  afconvert -f WAVE -d LEI16@16000 -c 1 {src} out.wav")
        if subprocess.run(["which", "afconvert"], capture_output=True).returncode != 0:
            print("提示：本机没有 afconvert（非 macOS），素材得自己先转成 16 kHz 单声道 WAV。")
        samples = trim_and_shape(read_wav(src))
    else:
        samples = synth_tone()
    emit(samples)


if __name__ == "__main__":
    main()
