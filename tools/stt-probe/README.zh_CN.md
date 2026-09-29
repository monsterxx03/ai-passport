<p align="right">
<a href="README.md">English</a> · <strong>简体中文</strong>
</p>

# stt-probe —— 主机侧转写能力的探针

语音输入那套方案（见 tachi 的 `docs/2026-09-29-badge-voice-answers-design.md`）里，
**转写发生在主机侧**，用的是 macOS 26 自己的 `Speech.framework`（不走 llm provider、不联网）。
这个目录是**拿来做决定的探针**——设计文档附录 B 里那些数字就是它们跑出来的。

留在这里的理由：那些数字是「量出来的」这个说法的唯一凭据。要复现、或者将来系统升级后
想确认结论还成立，跑一遍就行。

## 怎么跑

```sh
./run.sh 1      # 轮次见下表；第一次会弹「允许语音识别」，要同意
```

**必须走 `run.sh`（它会打包成 `.app` 再 `open`）**，不能直接 `swiftc && ./probe`：
TCC 把语音识别授权归因给 **responsible process**，裸 CLI 的 responsible process 是终端，
而终端没有 `NSSpeechRecognitionUsageDescription`——判定发生在**第一次调用**时，结果是**直接崩**。

跑完日志在 `logs/roundN.log`；`logs/` 里现存的那几份是最近一次跑出来的记录。

## 六轮各回答什么

| 轮次 | 问题 | 结论（写进了设计文档） |
| --- | --- | --- |
| **1** `round1_assets_and_locales.swift` | 有哪些语言可用、资产装没装、中英混说/双模块各是什么样 | `zh_CN`/`zh_TW` 预装、`en_US` 要下载；**双模块不融合**；`SpeechDetector` 只做 VAD 不判语种 |
| **2** `round2_bias_and_dictation.swift` | 偏置词表能不能救回英文词、Dictation 味道的转写器是否更合适 | **偏置词表完全无效**（加与不加一字不差）；`DictationTranscriber` 更差 |
| **3** `round3_controls_and_lossy.swift` | 对照实验：偏置到底有没有用；备选里有什么；ADPCM 往返 | **偏置无效**、备选只是尾部碎片 → 两条自动纠错路都堵死 |
| **4** `round4_sample_rate_lossy.swift` | 8 kHz 和 ADPCM 4:1 会不会把识别率打下去 | **ADPCM 不会**；**8 kHz 会**——长句上量不出差别，短句上结尾的英文词会整段塌成汉字（表 7）。所以 16 kHz 是首选，8 kHz 只是带宽不够时的退路 |
| **5** `round5_language_choice.swift` | 「有汉字就用 zh-CN，一个汉字都没有才比置信度」这条判据准不准 | **18 次里 17 次对**，唯一反例是纯英文句的一版（两个 conf 差 0.01，见下）——判据的第二层是脆的 |
| **6** `round6_streaming_input.swift` | 不落盘那条路（`AVAudioPCMBuffer` → `AsyncStream<AnalyzerInput>`）与文件模式是否等价 | **逐字一致（6/6）**、每块 512 与 1600 无差别 → 垫片的入参可以是「`[]int16` + 采样率」，全程不写磁盘 |

`adpcm_roundtrip.py` 是标准的 IMA ADPCM 编码→解码往返（4:1），第 3、4、5 轮都用它模拟
设备→主机那条有损链路。**它的实现就是设备端 `badge_voice.c` 将来该做的事**——两边各写一遍
是「协议两端各自实现」的代价，而这份 Python 是最好的参照。

## 第 5 轮复现出的一件事：纯英文句上判据会险胜出错

设计文档表 6 记的是「18 次全对」。这一轮的六句话是照文档补全的（文档里只写了前半句），
所以不是同一批音频，但它给出一个**文档里没有的失败样本**：

```
please revert that commit and rerun the tests       ← 纯英文句
  16k ADPCM 4:1   zh 0.56 / en 0.55   无汉字，比置信度 → 选 zh ✗
      zh: Plaser that comitenyroun the test.
      en: Please revert that commit, then rerun the tests.
```

两个模块的置信度**几乎并列（差 0.01）**，而 zh-CN 那串是音译垃圾、en-US 那句是原文。
也就是说：**「汉字优先」那层是稳的（中文句全中），脆的是第二层**——纯英文句上「比置信度」
可能被音译结果险胜。这条曲线要留在设计里：设备上大概率的输入是中文，代价可以接受，
但别把它当成一条可靠判据。

## 第 6 轮踩到的坑：别用 int16 的 buffer 直读 wav

第一版 `readSamples` 建了一个 `pcmFormatInt16` 的 `AVAudioPCMBuffer` 直接 `read(into:)`，
读出来的样本**每两个里有一个是 0**（`[0, -17768, 0, -17704, …]`），而这样的音频喂进转写器
只得到一句「是。」——看起来像「流式路径坏了」，其实是读音频那一步坏的。当时试了五种时序
（先灌满再 start、并发 start、带 `bufferStartTime`、一次喂整段、`analyzeSequence(流)`）
全是同一句「是。」，正好说明问题不在时序。正确姿势：读 float32，再自己按 32767 量化
（`AVAudioFile` 的 processingFormat 是 float32，它「帮你」转 int16 的结果不可信）。

## 三个刻意的设计

- **不用麦克风、不用人开口**：句子全部由 `say` 合成（Tingting / Samantha），再过 `afconvert`
  转成 16 kHz 单声道 Int16（和链路里要传的格式一致）。所以验证可以在没有人的时候跑。
- **每轮自足**：各自 `say` 合成自己要用的音频，产物写在本目录（`*.wav` / `*.aiff` 不入库）。
  轮次之间没有依赖，单跑任何一轮都行。
- **同一段音频的转写是逐次确定的**（第 4 轮每个变体跑 3 遍，输出逐字相同），所以「换一档更好/更差」
  是真的差别，不是抖动。但换一档音频（比如 8 kHz 无损）就会出现抖动：同一句同一音频三次给出
  `通 sh man` / `通 sman` / `通 shman`——**抖动本身就是那档质量不行的信号**。

## 这几轮**没有**证明的

**真机麦克风的声学**。`say` 合成的声音比「隔着一张桌子的 MEMS 麦克风 + 房间噪声」干净得多，
所以上面验的是**管线**（格式、压缩、语言、延迟），不是**声学**。声学那关等设备端能录音之后，
从电脑喇叭放这些合成音频让设备录一遍——那时才是端到端。
