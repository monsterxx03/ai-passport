// 第二轮：偏置词表(contextualStrings)能不能救回嵌入的英文词/技术词，
// 以及 Dictation 味道的转写器是否更合适，长句延迟如何。
import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round2.log")

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

// 项目里真实会说的词：工具名、文件名、缩写。
let vocabulary = [
    "tachi", "badge", "commit", "revert", "rerun", "push",
    "RSSI", "BLE", "battery level", "ESP-IDF", "LVGL", "go test", "git log",
]

final class Box: @unchecked Sendable { var text = "" }

func transcribe<M: SpeechModule>(
    clip: URL, module: M, bias: [String]?,
    extract: @escaping @Sendable (M.Result) -> String
) async -> (String, Int) {
    let analyzer = SpeechAnalyzer(modules: [module])
    if let b = bias {
        let ctx = AnalysisContext()
        ctx.contextualStrings[.general] = b
        do { try await analyzer.setContext(ctx) } catch { log("  设语境失败: \(error)") }
    }
    let box = Box()
    let task = Task { () -> Void in
        do {
            for try await r in module.results { box.text += extract(r) }
        } catch { box.text += "<结果流错误: \(error)>" }
    }
    let started = Date()
    do {
        _ = try await analyzer.analyzeSequence(from: try AVAudioFile(forReading: clip))
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch {
        log("  分析失败: \(error)")
    }
    _ = await task.value
    return (box.text, Int(Date().timeIntervalSince(started) * 1000))
}

struct Trial {
    let label: String
    let run: @Sendable (URL) async -> (String, Int)
}

func zhTrial(_ bias: [String]?) -> Trial {
    let m = SpeechTranscriber(locale: Locale(identifier: "zh-CN"), preset: .transcription)
    return Trial(label: bias == nil ? "zh 无偏置" : "zh 有偏置") { url in
        await transcribe(clip: url, module: m, bias: bias) { String($0.text.characters) }
    }
}

func enTrial(_ bias: [String]?) -> Trial {
    let m = SpeechTranscriber(locale: Locale(identifier: "en-US"), preset: .transcription)
    return Trial(label: bias == nil ? "en 无偏置" : "en 有偏置") { url in
        await transcribe(clip: url, module: m, bias: bias) { String($0.text.characters) }
    }
}

func dictTrial() -> Trial {
    let m = DictationTranscriber(locale: Locale(identifier: "zh-CN"), preset: .shortDictation)
    return Trial(label: "dict-zh   ") { url in
        await transcribe(clip: url, module: m, bias: nil) { String($0.text.characters) }
    }
}

let clips: [(String, String, String)] = [
    ("mixed", "把这个 commit revert 掉，然后 rerun 一下测试，最后 push 到 main", "Tingting"),
    ("tech", "看一下 badge 的 RSSI 和 battery level，还有 BLE 连接间隔", "Tingting"),
    ("long", "帮我看一下 desktop 那个 link 包里面的 ble 实现，我怀疑 read 返回之后没有检查连接状态，你把 go test 跑一下确认没有回归，然后提交一个 commit 推到 main 分支", "Tingting"),
]

@main
struct Probe2 {
    static func main() async {
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 第二轮：偏置词表 / Dictation 对比 ===")

        for id in ["zh-CN", "en-US"] {
            let t = SpeechTranscriber(locale: Locale(identifier: id), preset: .transcription)
            let fmts = await t.availableCompatibleAudioFormats
            log("\(id) 可接受格式: \(fmts.map { "\(Int($0.sampleRate))Hz/\($0.channelCount)ch/fmt=\($0.commonFormat.rawValue)" }.joined(separator: ", "))")
        }

        for (name, text, voice) in clips {
            let wav = dir.appendingPathComponent("\(name).wav")
            guard makeClip(text: text, voice: voice, to: wav) else { log("[\(name)] 合成失败"); continue }
            log("")
            log("[\(name)] 原句: \(text)")
            if let f = try? AVAudioFile(forReading: wav) {
                log("  时长 \(String(format: "%.1f", Double(f.length) / f.fileFormat.sampleRate))s")
            }
            for trial in [zhTrial(nil), zhTrial(vocabulary), enTrial(vocabulary), dictTrial()] {
                let (out, ms) = await trial.run(wav)
                log("  \(trial.label) [\(ms)ms] → \(out)")
            }
        }
        log("")
        log("=== 完 ===")
    }
}
