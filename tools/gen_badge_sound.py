# /// script
# dependencies = ["numpy"]
# ///
"""生成设备侧的提示音 PCM：main/badge_sound_ask_16k.c 与 main/badge_sound_done_16k.c。

    uv run tools/gen_badge_sound.py                                # 重新合成两段默认提示音
    uv run tools/gen_badge_sound.py assets/music/x.wav             # 用自己的素材当「有事等你」那一声
    uv run tools/gen_badge_sound.py assets/music/x.wav --kind done # ……当「回合完成」那一声

两段音频是**两件事**，所以是两份文件、两个符号（设备按主机发来的 kind 选）：

  ask   有事等你：权限确认，或者模型提了个问题——需要你现在动手
  done  一个回合跑完了：不需要你做什么，只是告诉你「好了」

素材本身放 assets/music/（见 assets/README.md 的「音乐与音效」），别跟 markdown 混在
assets/ 根目录下。

只吃 **WAV**：mp3/m4a 的解码要么拖进 ffmpeg 这个外部依赖，要么塞一个解码器进固件，而换
素材一年也就一两次——先用 macOS 自带的 afconvert 转一下就行：

    afconvert -f WAVE -d LEI16@16000 -c 1 my.m4a my.wav

输出是 16 kHz / 单声道 / 16-bit：BSP 的 bsp_audio_set_format(16000, 16, 1) 就是 demo 用的
那一档，人声与短铃在这个采样率上都够（采样率再高，这块小喇叭也放不出来）。

⚠ 素材的授权：脚本产出的是**入库**的（克隆下来直接就能构建）。所以换素材时要么用自己录的
/CC0 的，要么把产物留在本地别提交——用别人的录音（影视角色的语音之类）提交进仓库，等于把
它一起分发了。
"""
import os
import pathlib
import subprocess
import sys
import wave

import numpy as np

HZ = 16000
ROOT = pathlib.Path(__file__).resolve().parents[1]

# 两段默认音的性格刻意不同：向上的一声是「有事找你」，向下的两声是「好了，没你的事」。
# 都短（< 0.3 s）：提示音要说的是「有事」，不是把一首曲子放完。
TONE_PEAK = 0.55  # 满幅 1.0；留出余量，免得和系统音量叠加后削顶
FADE_SECONDS = 0.004  # 两端各淡入淡出，避免起止那一下「啪」

KINDS = {
    "ask": {
        "note": "向上的一声短铃（E6 + 一点二次谐波）",
        "segments": [(1318.5, 0.25, 0.06, 0.25)],  # (频率, 时长, 衰减, 谐波强度)
    },
    "done": {
        "note": "向下的两声（C6 → G5）",
        "segments": [(1046.5, 0.10, 0.05, 0.20), (784.0, 0.18, 0.06, 0.20)],
    },
}


def synth(kind: str) -> np.ndarray:
    parts = []
    for hz, seconds, decay, harmonic in KINDS[kind]["segments"]:
        n = int(HZ * seconds)
        t = np.arange(n) / HZ
        wave_ = np.sin(2 * np.pi * hz * t) + harmonic * np.sin(2 * np.pi * 2 * hz * t)
        env = np.exp(-t / decay)
        fade = max(1, int(HZ * FADE_SECONDS))
        ramp = np.linspace(0.0, 1.0, fade)
        env[:fade] *= ramp
        env[-fade:] *= ramp[::-1]
        parts.append(wave_ * env)
    data = np.concatenate(parts)
    return data / np.max(np.abs(data))


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
    return data / np.max(np.abs(data))


def source_label(path: str) -> str:
    """素材在仓库里就写相对路径：绝对路径（/Users/…）进不了提交，别人也读不到。

    仓库外的素材（自己录的一段）没有相对路径可写，就照实写绝对路径——它是本地生成的，
    提醒读的人「这一份不是从仓库里的素材来的」。
    """
    resolved = pathlib.Path(os.path.abspath(path))
    try:
        return str(resolved.relative_to(ROOT))
    except ValueError:
        return str(resolved)


def custom_sources() -> list[str]:
    """哪几种提示音现在装的是自定义素材——读生成文件头上那行「来源」。

    「来源」是生成器自己写的，所以它同时是「这份 PCM 从哪来」的唯一记录：素材文件删了、
    换了电脑，这行还在，而它说的是实话还是默认合成就靠它区分。
    """
    found = []
    for kind in KINDS:
        path = ROOT / "main" / f"badge_sound_{kind}_16k.c"
        if not path.exists():
            continue
        for line in path.read_text().splitlines()[:8]:
            if line.startswith("// 来源：") and "本脚本合成" not in line:
                found.append(f"{kind}（{line[len('// 来源：'):]}）")
                break
    return found


def emit(kind: str, samples: np.ndarray, note: str, source: str) -> None:
    out = str(ROOT / "main" / f"badge_sound_{kind}_16k.c")
    pcm = np.clip(samples * TONE_PEAK * 32767.0, -32768, 32767).astype("<i2")
    rows = [", ".join(str(int(v)) for v in pcm[i:i + 16]) for i in range(0, len(pcm), 16)]
    body = ",\n    ".join(rows)
    with open(out, "w") as f:
        f.write(f"""// 生成的提示音（tools/gen_badge_sound.py），别手改。
//
// {note} —— {len(pcm)} 个采样 = {len(pcm) / HZ:.2f} 秒 @ {HZ} Hz / 单声道 / 16-bit。
// 来源：{source}
// 换素材：uv run tools/gen_badge_sound.py <自己的 wav> --kind {kind}，然后重新构建。
#include "badge_sound.h"

const int16_t badge_sound_{kind}_pcm[] = {{
    {body}
}};

const size_t badge_sound_{kind}_samples = {len(pcm)};
""")
    print(f"已生成 {out}（{len(pcm)} 采样，{len(pcm) / HZ:.2f} 秒，"
          f"{os.path.getsize(out) // 1024} KB 源码）")


def main() -> None:
    args = sys.argv[1:]
    kind = "ask"
    path = None

    i = 0
    while i < len(args):
        if args[i] == "--kind":
            if i + 1 >= len(args) or args[i + 1] not in KINDS:
                raise SystemExit("--kind 只能是 " + " / ".join(KINDS))
            kind = args[i + 1]
            i += 2
            continue
        path = args[i]
        i += 1

    if path is None:
        # 不带参数：两段默认音都重新合成一遍（幂等）。
        #
        # 「默认」会盖掉已经装上的自定义素材，而这一步不可逆——素材要是自己录在电脑上的，
        # 盖完就没了。所以先说出来，别让人事后才发现那一声变了。
        replaced = custom_sources()
        if replaced:
            print("注意：会把已装的自定义素材换成默认合成音——" + "、".join(replaced), file=sys.stderr)
        for k in KINDS:
            emit(k, synth(k), KINDS[k]["note"], "本脚本合成")
        return

    if not os.path.exists(path):
        raise SystemExit(f"找不到输入文件：{path}")
    if not path.lower().endswith(".wav"):
        raise SystemExit("只吃 WAV，先转一下：\n"
                         f"  afconvert -f WAVE -d LEI16@16000 -c 1 {path} out.wav")
    if subprocess.run(["which", "afconvert"], capture_output=True).returncode != 0:
        print("提示：本机没有 afconvert（非 macOS），素材得自己先转成 16 kHz 单声道 WAV。")
    emit(kind, trim_and_shape(read_wav(path)), "自定义素材", source_label(path))


if __name__ == "__main__":
    main()
