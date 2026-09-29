// 第四轮：有损压缩（IMA ADPCM 4:1）和 8 kHz 采样率对识别率的影响。
//
// 这一轮自己合成音频、自己造四个变体，所以单独跑也行（不依赖前面几轮的产物）。
// 结论：8 kHz 与 16 kHz 没有可辨差别；ADPCM 4:1 在识别率上不付代价——由此定了
// 「8 kHz + IMA ADPCM = 4 KB/s」这个链路格式。
import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round4.log")

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

// say → aiff → 16 kHz / 单声道 / Int16（链路里要传的格式）。
func makeClip(text: String, voice: String, to wav: URL) -> Bool {
    let aiff = wav.deletingPathExtension().appendingPathExtension("aiff")
    run("/usr/bin/say", ["-v", voice, "-o", aiff.path, text])
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@16000", "-c", "1", aiff.path, wav.path])
    return FileManager.default.fileExists(atPath: wav.path)
}

// 造四个变体：16k/8k × 无损/ADPCM。
func makeVariants(_ base: String) -> [(String, String)] {
    let src = dir.appendingPathComponent("\(base).wav")
    let pcm8k = dir.appendingPathComponent("\(base)_8k.wav")
    let rt16 = dir.appendingPathComponent("\(base)_rt.wav")
    let rt8 = dir.appendingPathComponent("\(base)_8k_rt.wav")
    let py = dir.appendingPathComponent("adpcm_roundtrip.py")

    // 8 kHz 无损：重采样
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@8000", "-c", "1", src.path, pcm8k.path])
    // ADPCM 往返：走 adpcm_roundtrip.py（编码→解码），和将来设备→主机那条路同一件事
    for (from, to) in [(src, rt16), (pcm8k, rt8)] {
        run("/usr/bin/python3", [py.path, from.path, to.path])
    }

    return [
        ("16k 无损      ", src.path),
        ("16k ADPCM 4:1 ", rt16.path),
        ("8k  无损      ", pcm8k.path),
        ("8k  ADPCM 4:1 ", rt8.path),
    ]
}

final class Box: @unchecked Sendable { var text = ""; var alts: [String] = [] }

func recognize(_ path: String, alts: Bool = false) async -> (String, [String], Int) {
    let m = SpeechTranscriber(locale: Locale(identifier: "zh-CN"),
                              preset: alts ? .transcriptionWithAlternatives : .transcription)
    let analyzer = SpeechAnalyzer(modules: [m])
    let box = Box()
    let task = Task { () -> Void in
        do {
            for try await r in m.results {
                box.text += String(r.text.characters)
                if alts { box.alts = r.alternatives.map { String($0.characters) } }
            }
        } catch { box.text += "<结果流错误: \(error)>" }
    }
    let started = Date()
    do {
        _ = try await analyzer.analyzeSequence(from: try AVAudioFile(forReading: URL(fileURLWithPath: path)))
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch { log("    分析失败 \(path): \(error)") }
    _ = await task.value
    return (box.text, box.alts, Int(Date().timeIntervalSince(started) * 1000))
}

let clips: [(String, String)] = [
    ("lossy_long", "帮我看一下 desktop 那个 link 包里面的 ble 实现，我怀疑 read 返回之后没有检查连接状态，你把 go test 跑一下确认没有回归"),
    ("lossy_tech", "看一下 badge 的 RSSI 和 battery level，还有 BLE 连接间隔"),
    // 短句 + 嵌入式英文：这一句是 8 kHz 的软肋暴露得最明显的地方——长句上量不出差别，
    // 短句上英文词会整段被吞成汉字（见设计文档表 7）。
    ("lossy_short", "把这个 commit revert 掉，然后 rerun 一下测试，最后 push 到 main"),
]

// 每个变体跑几遍。转写不是逐次确定的（同一段音频两次的标点/词形会不同），所以
// 单跑一遍分不清「8 kHz 真的更差」和「这次运气不好」。
let repeats = 3

@main
struct Probe4 {
    static func main() async {
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("logs"),
                                                 withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 第四轮：有损压缩 / 采样率 ===")

        for (name, sentence) in clips {
            let wav = dir.appendingPathComponent("\(name).wav")
            guard makeClip(text: sentence, voice: "Tingting", to: wav) else {
                log("[\(name)] 合成失败（say / afconvert 不可用？）")
                continue
            }
            log("")
            log("[\(name)] 原句: \(sentence)")
            for (label, path) in makeVariants(name) {
                guard FileManager.default.fileExists(atPath: path) else {
                    log("  \(label) 变体缺失: \(path)"); continue
                }
                for round in 1...repeats {
                    let (out, _, ms) = await recognize(path)
                    log("  \(label) [\(ms)ms] #\(round) → \(out)")
                }
            }
        }

        log("")
        log("[alternatives] .transcriptionWithAlternatives 的备选里到底有什么")
        let tech = dir.appendingPathComponent("lossy_tech.wav").path
        if FileManager.default.fileExists(atPath: tech) {
            let (main, alts, _) = await recognize(tech, alts: true)
            log("  主结果: \(main)")
            for (i, a) in alts.enumerated() { log("  备选\(i + 1): \(a)") }
        }

        log("")
        log("=== 完 ===")
    }
}
