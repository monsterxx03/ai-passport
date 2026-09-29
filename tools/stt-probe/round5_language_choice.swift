// 第五轮：语言判据。
//
// 问题：人说的一句话是中英混杂的，而我们要一个模块出结果、且不许让「英文句被中文模块
// 音译成汉字」。设计文档 §10 定的判据是**两层**：zh-CN 的结果里出现汉字就用它；一个汉字
// 都没有时，才拿 zh-CN 和 en-US 的 transcriptionConfidence 比大小。
//
// 这一轮就是把那条判据放在六句话 × 三种音频（16k 无损 / 16k ADPCM / 8k ADPCM）上验一遍：
// 一个 analyzer 同时挂 zh + en 两个 transcriber（是「各自独立转完再选」，不是融合——
// SpeechDetector 只报有语音/无语音，不做语言识别），每边取最终文本与置信度。
//
// 为什么要两层而不是单比置信度，第 3 句的 8 kHz 那版就是答案：en-US 只抓到半句却给
// `Hi, R S S I, her battery level. Hi`（conf 0.67），zh-CN 0.80——只差 0.13 的裕度。
// 置信度说的是「我说得对不对」，不是「我听全了没有」。
import AVFoundation
import Foundation
import Speech

// 产物与日志写在本文件旁边（源码所在目录），所以从哪儿跑都一样。
let dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let logURL = dir.appendingPathComponent("logs/round5.log")

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

// 这一轮要的那三种：16k 无损 / 16k ADPCM / 8k ADPCM（8k 无损不在链条上，不测）。
func makeVariants(_ base: String) -> [(String, String)] {
    let src = dir.appendingPathComponent("\(base).wav")
    let pcm8k = dir.appendingPathComponent("\(base)_8k.wav")
    let rt16 = dir.appendingPathComponent("\(base)_rt.wav")
    let rt8 = dir.appendingPathComponent("\(base)_8k_rt.wav")
    let py = dir.appendingPathComponent("adpcm_roundtrip.py")

    run("/usr/bin/afconvert", ["-f", "WAVE", "-d", "LEI16@8000", "-c", "1", src.path, pcm8k.path])
    // ADPCM 往返：编码→解码，和将来设备→主机那条有损链路是同一件事。
    run("/usr/bin/python3", [py.path, src.path, rt16.path])
    run("/usr/bin/python3", [py.path, pcm8k.path, rt8.path])

    return [
        ("16k 无损      ", src.path),
        ("16k ADPCM 4:1 ", rt16.path),
        ("8k  ADPCM 4:1 ", rt8.path),
    ]
}

final class Box: @unchecked Sendable {
    var text = ""
    var conf: Double = 0
}

// 置信度是挂在结果 AttributedString 上的属性（要显式申请：`.transcriptionConfidence`
// 不是 preset 的默认项，也不是 Result 的字段），而且是**按 run 分段**报的。
// 这里取各段里的**最小值**：一句话里有一段没把握，整句就不能算有把握。
func confidence(of text: AttributedString) -> Double? {
    text.runs.compactMap { $0[AttributeScopes.SpeechAttributes.ConfidenceAttribute.self] }.min()
}

func transcriber(_ id: String) -> SpeechTranscriber {
    SpeechTranscriber(locale: Locale(identifier: id),
                      transcriptionOptions: [],
                      reportingOptions: [],
                      attributeOptions: [.transcriptionConfidence])
}

// 一个 analyzer 挂 zh + en 两个模块，两边各自转写整段音频。置信度取最后一个结果里各 run 的最小值。
func recognizeBoth(path: String) async
    -> (zh: String, zhConf: Double, en: String, enConf: Double, ms: Int)
{
    let zh = transcriber("zh-CN")
    let en = transcriber("en-US")
    let analyzer = SpeechAnalyzer(modules: [zh, en])
    let zhBox = Box(), enBox = Box()

    func collect(_ m: SpeechTranscriber, _ box: Box) -> Task<Void, Never> {
        Task {
            do {
                for try await r in m.results {
                    box.text += String(r.text.characters)
                    if let c = confidence(of: r.text) { box.conf = c }
                }
            } catch { box.text += "<结果流错误: \(error)>" }
        }
    }
    let zhTask = collect(zh, zhBox)
    let enTask = collect(en, enBox)

    let started = Date()
    do {
        _ = try await analyzer.analyzeSequence(from: try AVAudioFile(forReading: URL(fileURLWithPath: path)))
        try await analyzer.finalizeAndFinishThroughEndOfInput()
    } catch { log("    分析失败 \(path): \(error)") }
    _ = await zhTask.value
    _ = await enTask.value
    return (zhBox.text, zhBox.conf, enBox.text, enBox.conf, Int(Date().timeIntervalSince(started) * 1000))
}

// 判据第一层：结果里有没有汉字。
func hasHan(_ s: String) -> Bool {
    s.unicodeScalars.contains { (0x3400...0x4DBF).contains($0.value) || (0x4E00...0x9FFF).contains($0.value) }
}

// 判据本身，和将来要写进 stt 垫片的那份是同一套规则。
// 返回（选了谁, 为什么, 单比置信度会选谁）。
func decide(zh: String, zhConf: Double, en: String, enConf: Double) -> (String, String, String) {
    let byConf = zhConf >= enConf ? "zh" : "en"
    if hasHan(zh) { return ("zh", "有汉字", byConf) }
    return (byConf, String(format: "无汉字，比置信度 %.2f vs %.2f", zhConf, enConf), byConf)
}

// (id, 句, 音色, 期望语言)
let clips: [(String, String, String, String)] = [
    ("lang_zh_1", "帮我把这个改动提交一下，顺便写一句 commit message", "Tingting", "zh"),
    ("lang_zh_2", "把这个 commit revert 掉，然后 rerun 一下测试，最后 push 到 main", "Tingting", "zh"),
    ("lang_zh_3", "看一下 badge 的 RSSI 和 battery level，还有 BLE 连接间隔", "Tingting", "zh"),
    ("lang_zh_4", "先跑一下 lint，然后 make test 确认没有回归", "Tingting", "zh"),
    ("lang_en_1", "please revert that commit and rerun the tests", "Samantha", "en"),
    ("lang_en_2", "list the changed files and show me the diff", "Samantha", "en"),
]

@main
struct Probe5 {
    static func main() async {
        try? FileManager.default.createDirectory(at: dir.appendingPathComponent("logs"),
                                                 withIntermediateDirectories: true)
        try? FileManager.default.removeItem(at: logURL)
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        log("=== 第五轮：语言判据（汉字优先，否则比置信度）===")

        // en-US 的资产要装了才谈得上判据；没装就直说，别让 en 那列静默变成空串。
        let installed = await SpeechTranscriber.installedLocales.map(\.identifier)
        log("已装资产: \(installed.sorted().joined(separator: ","))")
        for id in ["zh-CN", "en-US"] where !installed.contains(id.replacingOccurrences(of: "-", with: "_")) {
            log("⚠ \(id) 的资产没装：这一轮的结果不成立（先让它下载一次）")
        }

        var total = 0, correct = 0, confOnlyAgrees = 0
        for (name, sentence, voice, expected) in clips {
            let wav = dir.appendingPathComponent("\(name).wav")
            guard makeClip(text: sentence, voice: voice, to: wav) else {
                log("[\(name)] 合成失败（say / afconvert 不可用？）")
                continue
            }
            log("")
            log("[\(name)] 期望 \(expected)  原句: \(sentence)")
            for (label, path) in makeVariants(name) {
                guard FileManager.default.fileExists(atPath: path) else {
                    log("  \(label) 变体缺失: \(path)"); continue
                }
                let r = await recognizeBoth(path: path)
                let (choice, why, byConf) = decide(zh: r.zh, zhConf: r.zhConf,
                                                   en: r.en, enConf: r.enConf)
                total += 1
                if choice == expected { correct += 1 }
                if byConf == choice { confOnlyAgrees += 1 }
                log(String(format: "  %@ [%dms] zh %.2f / en %.2f  %@ → %@ %@",
                           label, r.ms, r.zhConf, r.enConf, why, choice,
                           choice == expected ? "✓" : "✗ 错（期望 \(expected)）"))
                log("      zh: \(r.zh)")
                log("      en: \(r.en)")
            }
        }

        log("")
        log("判据: \(correct)/\(total) 对；单比置信度会做出一致选择的有 \(confOnlyAgrees)/\(total)")
        log("")
        log("=== 完 ===")
    }
}
