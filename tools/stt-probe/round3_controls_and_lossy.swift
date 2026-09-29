// 第三轮：三个决定设计的问题
//   A. contextualStrings 到底有没有用（用一句几乎无法猜对的句子做对照）
//   B. alternatives 里有没有正确答案（有的话可以零成本纠错）
//   C. 设备端的 IMA ADPCM 4:1 往返，会不会把识别率打下去
import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round3.log")

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

// 模拟设备链路：16k 单声道 PCM → IMA ADPCM 4:1 → 主机解回 PCM
func adpcmRoundTrip(_ src: URL, _ dst: URL) -> Bool {
    let enc = dir.appendingPathComponent("_rt_enc.wav")
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "ima4@16000", "-c", "1", src.path, enc.path])
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@16000", "-c", "1", enc.path, dst.path])
    return FileManager.default.fileExists(atPath: dst.path)
}

final class Box: @unchecked Sendable {
    var text = ""
    var alts: [String] = []
}

func transcribe<M: SpeechModule>(
    clip: URL, module: M, bias: [String]? = nil, wantAlternatives: Bool = false,
    extract: @escaping @Sendable (M.Result) -> String,
    alts: @escaping @Sendable (M.Result) -> [String]
) async -> (String, [String], Int) {
    let analyzer = SpeechAnalyzer(modules: [module])
    if let b = bias {
        let ctx = AnalysisContext()
        ctx.contextualStrings[.general] = b
        do { try await analyzer.setContext(ctx) } catch { log("  设语境失败: \(error)") }
    }
    let box = Box()
    let task = Task { () -> Void in
        do {
            for try await r in module.results {
                box.text += extract(r)
                if wantAlternatives, box.alts.isEmpty { box.alts = alts(r) }
            }
        } catch { box.text += "<结果流错误: \(error)>" }
    }
    let started = Date()
    do {
        _ = try await analyzer.analyzeSequence(from: try AVAudioFile(forReading: clip))
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch { log("  分析失败: \(error)") }
    _ = await task.value
    return (box.text, box.alts, Int(Date().timeIntervalSince(started) * 1000))
}

func zh(_ preset: SpeechTranscriber.Preset = .transcription) -> SpeechTranscriber {
    SpeechTranscriber(locale: Locale(identifier: "zh-CN"), preset: preset)
}

func zhRun(_ clip: URL, _ m: SpeechTranscriber, bias: [String]?, wantAlts: Bool = false) async -> (String, [String], Int) {
    await transcribe(clip: clip, module: m, bias: bias, wantAlternatives: wantAlts,
                     extract: { String($0.text.characters) },
                     alts: { $0.alternatives.map { String($0.characters) } })
}

@main
struct Probe3 {
    static func main() async {
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 第三轮：偏置 / 备选 / 有损压缩 ===")

        // --- A. contextualStrings 的对照实验 ---
        // 选一句中文模块几乎不可能猜对的专有名词组合。
        let hard = "打开 tachibadge 的 kubeconfig，把 rasterizer 那个 pod 重启一下"
        let wavA = dir.appendingPathComponent("hard.wav")
        if makeClip(text: hard, voice: "Tingting", to: wavA) {
            log("")
            log("[A] 原句: \(hard)")
            let noBias = await zhRun(wavA, zh(), bias: nil)
            log("  无偏置 [\(noBias.2)ms] → \(noBias.0)")
            let withBias = await zhRun(wavA, zh(), bias: ["tachibadge", "kubeconfig", "rasterizer", "pod"])
            log("  有偏置 [\(withBias.2)ms] → \(withBias.0)")
        }

        // --- B. alternatives ---
        let techText = "看一下 badge 的 RSSI 和 battery level，还有 BLE 连接间隔"
        let wavB = dir.appendingPathComponent("tech.wav")
        if makeClip(text: techText, voice: "Tingting", to: wavB) {
            log("")
            log("[B] 原句: \(techText)")
            let r = await zhRun(wavB, zh(.transcriptionWithAlternatives), bias: nil, wantAlts: true)
            log("  主结果 [\(r.2)ms] → \(r.0)")
            for (i, a) in r.1.enumerated() { log("  备选\(i + 1): \(a)") }
        }

        // --- C. IMA ADPCM 4:1 往返 ---
        let longText = "帮我看一下 desktop 那个 link 包里面的 ble 实现，我怀疑 read 返回之后没有检查连接状态，你把 go test 跑一下确认没有回归"
        let wavC = dir.appendingPathComponent("longc.wav")
        let wavRT = dir.appendingPathComponent("longc_rt.wav")
        if makeClip(text: longText, voice: "Tingting", to: wavC) {
            log("")
            log("[C] 原句: \(longText)")
            let clean = await zhRun(wavC, zh(), bias: nil)
            log("  原始 16k PCM   [\(clean.2)ms] → \(clean.0)")
            if adpcmRoundTrip(wavC, wavRT) {
                let sz = (try? FileManager.default.attributesOfItem(atPath: wavRT.path)[.size] as? Int) ?? 0
                let enc = dir.appendingPathComponent("_rt_enc.wav")
                let encSz = (try? FileManager.default.attributesOfItem(atPath: enc.path)[.size] as? Int) ?? 0
                log("  ADPCM 体积 \(encSz) 字节 vs 解回 PCM \(sz) 字节")
                let lossy = await zhRun(wavRT, zh(), bias: nil)
                log("  ADPCM 往返后   [\(lossy.2)ms] → \(lossy.0)")
            } else {
                log("  ADPCM 往返失败（本机 afconvert 不支持 ima4）")
            }
        }

        log("")
        log("=== 完 ===")
    }
}
