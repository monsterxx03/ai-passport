// 第六轮：流式入口——不落盘那条路。
//
// 设计文档 §8 承诺了「音频不落盘」，所以正式实现不能走表 3/5 用过的
// `analyzeSequence(from: AVAudioFile)`（那条要先有一个音频文件）。这一轮验的是另一条：
// 把裸 Int16 样本包成 AVAudioPCMBuffer → 喂 AsyncStream<AnalyzerInput> →
// start(inputSequence:) → finalizeAndFinishThroughEndOfInput()。
//
// 两件事要在这里定下来：
//   1. 流式的结果与文件模式**逐字一致**（否则「不落盘」是个降级，不是等价替换）；
//   2. 每块喂 512 还是 1600 个样本**没有差别**（分块大小不该成为一条隐藏的调参）。
//
// 于是垫片的入参可以就是「[]int16 + 采样率」，全程不碰磁盘。
//
// ⚠ 这一轮自己踩到的坑（正式实现里千万别重犯）：**别用 int16 的 AVAudioPCMBuffer
// 直接 `read(into:)` 去读 wav**。AVAudioFile 的 processingFormat 是 float32，它「帮」你
// 转成 int16 的结果是坏的——实测每两个样本里有一个是 0（`[0, -17768, 0, -17704, …]`），
// 而坏音频喂进转写器只会得到一句「是。」，且看上去像「流式路径有问题」。
// 正确姿势见 readSamples：读 float32，再自己按 32767 量化。
import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round6.log")

func log(_ line: String) {
    FileHandle.standardOutput.write((line + "\n").data(using: .utf8)!)
    guard let h = try? FileHandle(forWritingTo: logURL) else { return }
    h.seekToEndOfFile()
    h.write((line + "\n").data(using: .utf8)!)
    try? h.close()
}

func run(_ tool: String, _ args: [String]) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: tool)
    p.arguments = args
    p.standardError = FileHandle.nullDevice
    try? p.run()
    p.waitUntilExit()
}

func makeClip(text: String, voice: String, to wav: URL) -> Bool {
    let aiff = wav.deletingPathExtension().appendingPathExtension("aiff")
    run("/usr/bin/say", ["-v", voice, "-o", aiff.path, text])
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff.path, wav.path])
    return FileManager.default.fileExists(atPath: wav.path)
}

final class Box: @unchecked Sendable { var text = "" }

func collect(_ m: SpeechTranscriber, _ box: Box) -> Task<Void, Never> {
    Task {
        do { for try await r in m.results { box.text += String(r.text.characters) } }
        catch { box.text += "<结果流错误: \(error)>" }
    }
}

// 文件模式：表 3/5 用过的那条，这里只当对照的基线。
func transcribeFile(_ path: String) async -> (String, Int) {
    let m = SpeechTranscriber(locale: Locale(identifier: "zh-CN"), preset: .transcription)
    let analyzer = SpeechAnalyzer(modules: [m])
    let box = Box()
    let task = collect(m, box)
    let started = Date()
    do {
        _ = try await analyzer.analyzeSequence(from: try AVAudioFile(forReading: URL(fileURLWithPath: path)))
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch { log("  分析失败 \(path): \(error)") }
    _ = await task.value
    return (box.text, Int(Date().timeIntervalSince(started) * 1000))
}

// 把 wav 读成裸 Int16 样本——将来从链路里解出来的就是这个形状。
// 先读 float32 再自己量化，理由见文件顶上那个坑。
func readSamples(_ url: URL) -> (samples: [Int16], rate: Double)? {
    guard let file = try? AVAudioFile(forReading: url) else { return nil }
    let fmt = file.processingFormat
    guard let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(file.length))
    else { return nil }
    do { try file.read(into: buf) } catch { log("  读取失败: \(error)"); return nil }
    guard let fp = buf.floatChannelData?[0] else { return nil }
    let n = Int(buf.frameLength)
    var out = [Int16](repeating: 0, count: n)
    for i in 0..<n {
        let v = (fp[i] * 32767).rounded()
        out[i] = Int16(max(-32768, min(32767, v)))
    }
    return (out, fmt.sampleRate)
}

// 流式那条路。时序上让 start 与灌数据**并发**：不用赌 start 是「立即返回」还是
// 「消费到流结束才返回」——AsyncStream 有缓冲，两种语义下都对。
func transcribeStream(_ samples: [Int16], rate: Double, chunk: Int) async -> (String, Int) {
    let m = SpeechTranscriber(locale: Locale(identifier: "zh-CN"), preset: .transcription)
    let analyzer = SpeechAnalyzer(modules: [m])
    let box = Box()
    let task = collect(m, box)

    // Int16 / 单声道 / 非交错——表 1 说这就是模块的原生输入格式。
    let fmt = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: rate,
                            channels: 1, interleaved: false)!
    let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()

    let started = Date()
    let starter = Task { try await analyzer.start(inputSequence: stream) }
    var i = 0
    while i < samples.count {
        let n = min(chunk, samples.count - i)
        guard let buf = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(n)) else { break }
        buf.frameLength = AVAudioFrameCount(n)
        samples.withUnsafeBufferPointer { src in
            buf.int16ChannelData![0].update(from: src.baseAddress! + i, count: n)
        }
        continuation.yield(AnalyzerInput(buffer: buf))
        i += n
    }
    continuation.finish()

    do {
        try await starter.value
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch { log("  流式分析失败: \(error)") }
    _ = await task.value
    return (box.text, Int(Date().timeIntervalSince(started) * 1000))
}

let sentence = "帮我看一下 desktop 那个 link 包里面的 ble 实现，我怀疑 read 返回之后没有检查连接状态，你把 go test 跑一下确认没有回归"

@main
struct Probe6 {
    static func main() async {
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("logs"),
                                                 withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 第六轮：流式入口（不落盘那条路）===")

        let wav = dir.appendingPathComponent("round6_long.wav")
        guard makeClip(text: sentence, voice: "Tingting", to: wav),
              let (samples, rate) = readSamples(wav) else {
            log("合成或读取失败（say / afconvert 不可用？）")
            return
        }
        log("音频: \(wav.lastPathComponent)  \(String(format: "%.1f", Double(samples.count) / rate))s / \(Int(rate)) Hz / 单声道 Int16，共 \(samples.count) 样本")
        log("")

        var baseline: String?
        log("[文件模式 · 基线] analyzeSequence(from: AVAudioFile)")
        for round in 1...2 {
            let (out, ms) = await transcribeFile(wav.path)
            if baseline == nil { baseline = out }
            let note = round == 1 ? "（进程内首次调用，含冷启动）" : ""
            log("  #\(round) [\(ms)ms]\(note) → \(out)")
        }

        var streamed: [(String, String, Int)] = []   // (标签, 文本, ms)
        log("")
        log("[流式] AVAudioPCMBuffer → AsyncStream<AnalyzerInput> → start(inputSequence:) → finalizeAndFinishThroughEndOfInput()")
        for chunk in [512, 1600] {
            for round in 1...3 {
                let (out, ms) = await transcribeStream(samples, rate: rate, chunk: chunk)
                streamed.append(("每块 \(chunk) 样本 #\(round)", out, ms))
                log("  \(String(format: "%-16@", "每块 \(chunk) 样本 #\(round)" as NSString)) [\(ms)ms] → \(out)")
            }
        }

        log("")
        log("[逐字比较] 流式 vs 文件模式基线")
        if let base = baseline {
            var same = 0
            for (label, out, _) in streamed {
                let ok = out == base
                if ok { same += 1 }
                log("  \(label)：\(ok ? "逐字一致 ✓" : "不一致 ✗\n      基线: \(base)\n      这次: \(out)")")
            }
            log("  \(same)/\(streamed.count) 与基线逐字一致")
            let texts = Set(streamed.map { $0.1 })
            log("  块大小 512 vs 1600：结果\(texts.count == 1 ? "无差别 ✓" : "有差别 ✗（\(texts.count) 种）")")
        }

        log("")
        log("=== 完 ===")
    }
}
