// 语音识别探针：验证 macOS 26 的 SpeechAnalyzer/SpeechTranscriber 能不能满足我们的用法。
//
// 它回答三个问题（都不用你开口——句子是 say 合成的）：
//   1. 这台机器上，zh-CN / en-US 的**本机**识别资产装了没有（AssetInventory）
//   2. 中英混说的句子，zh-CN 单一 locale 能识别成什么样；en-US 又成什么样
//   3. 一个 analyzer 里同时挂 zh + en 两个 transcriber 行不行（代码切换的正经解）
//
// 必须打包成 .app 跑：TCC 把语音识别授权归因给 **responsible process**，裸 CLI 是终端，
// 而终端没有 NSSpeechRecognitionUsageDescription——判定发生在第一次调用时，结果是直接崩。
// 用法见同目录 README.md：必须打包成 .app 再 open（TCC 归因 responsible process）。

import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round1.log")

func log(_ line: String) {
    FileHandle.standardOutput.write((line + "\n").data(using: .utf8)!)
    guard let h = try? FileHandle(forWritingTo: logURL) else { return }
    h.seekToEndOfFile()
    h.write((line + "\n").data(using: .utf8)!)
    try? h.close()
}

let clips: [(String, String, String)] = [
    ("zh", "帮我把这个改动提交一下，顺便看看有没有测试没跑", "Tingting"),
    ("mixed", "把这个 commit revert 掉，然后 rerun 一下测试，最后 push 到 main", "Tingting"),
    ("en", "please revert that commit and run the tests again before pushing", "Samantha"),
    ("tech", "看一下 badge 的 RSSI 和 battery level，还有 BLE 连接间隔", "Tingting"),
]

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
    // 转成和链路里同样的格式：16 kHz / 单声道 / 16-bit。
    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@16000", "-c", "1",
                              aiff.path, wav.path])
    return FileManager.default.fileExists(atPath: wav.path)
}

func transcribe(clip: URL, locales: [String]) async -> [(String, String)] {
    var transcribers: [SpeechTranscriber] = []
    for id in locales {
        transcribers.append(SpeechTranscriber(locale: Locale(identifier: id), preset: .transcription))
    }
    // 资产：没装就先下载（这一步可能联网、可能很慢）。
    do {
        let status = await AssetInventory.status(forModules: transcribers)
        log("  资产状态(\(locales.joined(separator: "+"))): \(String(describing: status))")
        if let req = try await AssetInventory.assetInstallationRequest(supporting: transcribers) {
            log("  正在安装识别资产…")
            try await req.downloadAndInstall()
        }
    } catch {
        log("  资产处理失败: \(error)")
        return locales.map { ($0, "<资产失败>") }
    }

    let analyzer = SpeechAnalyzer(modules: transcribers)
    var collected: [Int: String] = [:]
    var tasks: [Task<Void, Never>] = []
    for (i, t) in transcribers.enumerated() {
        tasks.append(Task {
            do {
                for try await r in t.results {
                    collected[i, default: ""] += String(r.text.characters)
                }
            } catch {
                collected[i, default: ""] += "<结果流错误: \(error)>"
            }
        })
    }

    let started = Date()
    do {
        let file = try AVAudioFile(forReading: clip)
        _ = try await analyzer.analyzeSequence(from: file)
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch {
        log("  分析失败: \(error)")
    }
    for t in tasks { _ = await t.value }
    let ms = Int(Date().timeIntervalSince(started) * 1000)
    log("  用时 \(ms) ms")

    return transcribers.enumerated().map { (i, _) in (locales[i], collected[i] ?? "") }
}

@main
struct Probe {
    static func main() async {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 语音识别探针 ===")

        let status: SFSpeechRecognizerAuthorizationStatus = await withCheckedContinuation { c in
            SFSpeechRecognizer.requestAuthorization { c.resume(returning: $0) }
        }
        log("授权状态: \(status.rawValue) （3 = 已授权）")
        guard status == .authorized else {
            log("没授权，退出。去 系统设置 → 隐私与安全性 → 语音识别 里勾上这个 probe。")
            return
        }

        for id in ["zh-CN", "en-US"] {
            let t = SpeechTranscriber(locale: Locale(identifier: id), preset: .transcription)
            let s = await AssetInventory.status(forModules: [t])
            log("  \(id): \(String(describing: s))")
        }
        log("已装资产的语言: \(await SpeechTranscriber.installedLocales.map(\.identifier).joined(separator: ", "))")
        log("支持的语言数: \(await SpeechTranscriber.supportedLocales.count)")

        for (name, text, voice) in clips {
            let wav = dir.appendingPathComponent("\(name).wav")
            guard makeClip(text: text, voice: voice, to: wav) else {
                log("[\(name)] 合成失败，跳过")
                continue
            }
            log("")
            log("[\(name)] 原句: \(text)")
            for mode in [["zh-CN"], ["en-US"], ["zh-CN", "en-US"]] {
                let out = await transcribe(clip: wav, locales: mode)
                for (locale, text) in out {
                    log("  \(locale) → \(text)")
                }
            }
        }
        log("")
        log("=== 完 ===")
    }
}
