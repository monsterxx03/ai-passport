# stt-probe —— 主机侧转写能力的探针

语音回答那套方案（见 tachi 的 `docs/2026-09-29-badge-voice-answers-design.md`）里，
**转写发生在主机侧**，用的是 macOS 26 自己的 `Speech.framework`（不走 llm provider、不联网）。
这个目录是**当初拿来做决定的四轮探针**——设计文档附录 B 里那些数字就是它们跑出来的。

留在这里的理由：那些数字是「量出来的」这个说法的唯一凭据。要复现、或者将来系统升级后
想确认结论还成立，跑一遍就行。

## 怎么跑

```sh
./run.sh 1      # 轮次见下表；第一次会弹「允许语音识别」，要同意
```

**必须走 `run.sh`（它会打包成 `.app` 再 `open`）**，不能直接 `swiftc && ./probe`：
TCC 把语音识别授权归因给 **responsible process**，裸 CLI 的 responsible process 是终端，
而终端没有 `NSSpeechRecognitionUsageDescription`——判定发生在**第一次调用**时，结果是**直接崩**。

跑完日志在 `logs/roundN.log`；`logs/` 里现存的那四份是设计文档定稿时的那次记录。

## 四轮各回答什么

| 轮次 | 问题 | 结论（写进了设计文档） |
| --- | --- | --- |
| **1** `round1_assets_and_locales.swift` | 有哪些语言可用、资产装没装、中英混说/双模块各是什么样 | `zh_CN`/`zh_TW` 预装、`en_US` 要下载；**双模块不融合**；`SpeechDetector` 只做 VAD 不判语种 |
| **2** `round2_bias_and_dictation.swift` | 偏置词表能不能救回英文词、Dictation 味道的转写器是否更合适 | **偏置词表完全无效**（加与不加一字不差）；`DictationTranscriber` 更差 |
| **3** `round3_controls_and_lossy.swift` | 对照实验：偏置到底有没有用；备选里有什么；ADPCM 往返 | **偏置无效**、备选只是尾部碎片 → 两条自动纠错路都堵死 |
| **4** `round4_sample_rate_lossy.swift` | 8 kHz 和 ADPCM 4:1 会不会把识别率打下去 | **都不会** → 链路格式定成 8 kHz + IMA ADPCM = **4 KB/s** |

`adpcm_roundtrip.py` 是标准的 IMA ADPCM 编码→解码往返（4:1），第 3、4 轮都用它模拟
设备→主机那条有损链路。**它的实现就是设备端 `badge_voice.c` 将来该做的事**——两边各写一遍
是「协议两端各自实现」的代价，而这份 Python 是最好的参照。

## 两个刻意的设计

- **不用麦克风、不用人开口**：句子全部由 `say` 合成（Tingting / Samantha），再过 `afconvert`
  转成 16 kHz 单声道 Int16（和链路里要传的格式一致）。所以验证可以在没有人的时候跑。
- **每轮自足**：各自 `say` 合成自己要用的音频，产物写在本目录（`*.wav` / `*.aiff` 不入库）。
  轮次之间没有依赖，单跑任何一轮都行。

## 这四轮**没有**证明的

**真机麦克风的声学**。`say` 合成的声音比「隔着一张桌子的 MEMS 麦克风 + 房间噪声」干净得多，
所以上面验的是**管线**（格式、压缩、语言、延迟），不是**声学**。声学那关等设备端能录音之后，
从电脑喇叭放这些合成音频让设备录一遍——那时才是端到端。
