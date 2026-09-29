<p align="right">
<a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# stt-probe — host-side transcription probes

The badge voice-input design (tachi's `docs/2026-09-29-badge-voice-answers-design.md`) puts
**transcription on the host**, using macOS 26's own `Speech.framework` (no LLM provider, no
network). This directory holds **the probes that decided that design** — the numbers in its
appendix B came from these scripts.

They stay in the tree because they are the only evidence behind the claim "those numbers were
measured, not guessed". Re-run one to reproduce a result, or to check the conclusions still
hold after an OS upgrade.

## Running

```sh
./run.sh 1      # round N, see the table below; the first run asks for Speech Recognition
```

**It has to go through `run.sh`** (it packages a `.app` and `open`s it); a bare
`swiftc && ./probe` does NOT work: TCC attributes speech-recognition authorization to the
**responsible process**, and for a bare CLI that is the terminal — which has no
`NSSpeechRecognitionUsageDescription`. The verdict happens on the **first call**, and the
result is a **crash**, not a permission prompt.

Logs land in `logs/roundN.log`; the copies in the tree are from the most recent run.

## What each round answers

| Round | Question | Conclusion (and where it landed) |
| --- | --- | --- |
| **1** `round1_assets_and_locales.swift` | which locales exist, are the assets installed, what do mixed zh/en and two modules look like | `zh_CN`/`zh_TW` are preinstalled, `en_US` needs a download; **two modules do not fuse**; `SpeechDetector` is VAD only, it does not identify a language |
| **2** `round2_bias_and_dictation.swift` | can a bias word list rescue embedded English words, and is the Dictation-flavoured transcriber better | **the bias list does nothing** (identical output with and without it); `DictationTranscriber` is worse |
| **3** `round3_controls_and_lossy.swift` | controlled experiment: does bias work at all, what is in `alternatives`, ADPCM round trip | **bias is inert**, alternatives are tail fragments only → both automatic correction routes are dead ends |
| **4** `round4_sample_rate_lossy.swift` | do 8 kHz and ADPCM 4:1 hurt recognition | **ADPCM does not**; **8 kHz does** — indistinguishable on a long sentence, but on a short one the trailing English words collapse into Chinese characters (table 7). So 16 kHz is the default and 8 kHz is only the fallback when bandwidth is short |
| **5** `round5_language_choice.swift` | is the rule "use zh-CN whenever its output has a Chinese character, otherwise compare confidence" accurate | **17 of 18 correct**; the single miss is an English-only sentence where the two confidences differ by 0.01 (below) — the second layer of the rule is fragile |
| **6** `round6_streaming_input.swift` | is the no-temp-file path (`AVAudioPCMBuffer` → `AsyncStream<AnalyzerInput>`) equivalent to the file path | **byte-identical output (6/6)**, and 512- vs 1600-sample chunks make no difference → the shim's input can be "`[]int16` plus sample rate", never touching disk |

`adpcm_roundtrip.py` is a standard IMA ADPCM encode→decode round trip (4:1); rounds 3, 4 and 5
use it to simulate the lossy device→host link. **It does what the device-side `badge_voice.c`
will have to do** — writing it twice is the price of implementing both ends of a protocol, and
this Python is the best reference for the second implementation.

## What round 5 turned up: on an English-only sentence the rule can lose by a hair

The design document records "18 of 18 correct" for table 6. Round 5's six sentences were
reconstructed from that table (which only prints the first half of each sentence), so this is
not the same audio — but it produced a **failure sample the document does not have**:

```
please revert that commit and rerun the tests       <- English-only sentence
  16k ADPCM 4:1   zh 0.56 / en 0.55   no Chinese character, compare confidence -> picked zh (WRONG)
      zh: Plaser that comitenyroun the test.
      en: Please revert that commit, then rerun the tests.
```

The two confidences are **almost tied (0.01 apart)**, and the zh-CN string is transliteration
garbage while the en-US string is the actual sentence. In other words: **the "Chinese character
first" layer is solid (every Chinese sentence was right); the fragile layer is the second one**
— on an English-only sentence, "compare confidence" can be won by transliteration. Keep that
curve in the design: the overwhelmingly likely device input is Chinese, so this cost is
acceptable, but do not treat the second layer as a reliable rule.

## The trap round 6 hit: do not read a wav into an int16 buffer

The first `readSamples` built a `pcmFormatInt16` `AVAudioPCMBuffer` and called `read(into:)` on
it directly. The samples came out with **every other one zero** (`[0, -17768, 0, -17704, …]`),
and audio like that transcribes to a single word — which looks exactly like "the streaming path
is broken" while the real damage is in reading the file. Five different orderings of the same
experiment (fill then start, start concurrently, with `bufferStartTime`, one whole buffer,
`analyzeSequence(stream)`) all produced that same single word, which is what proved the ordering
was not the problem. The right way: read float32 and quantize to int16 yourself (`AVAudioFile`'s
processingFormat is float32, and its "helpful" conversion to int16 is not trustworthy).

## Three deliberate choices

- **No microphone, nobody has to speak**: every sentence is synthesized by `say` (Tingting /
  Samantha) and converted by `afconvert` to 16 kHz mono Int16 (the format the link will carry).
  So the checks run when nobody is around.
- **Each round is self-contained**: every round synthesizes its own audio and writes its
  artifacts into this directory (`*.wav` / `*.aiff` are not committed). There are no
  dependencies between rounds; any round can be run on its own.
- **Transcription of a given clip is repeatable** (round 4 ran each variant 3 times with
  byte-identical output), so "one setting is better/worse" is a real difference, not jitter.
  Changing the audio, though, does introduce jitter: the same sentence at 8 kHz lossless gave
  three different endings across three runs — and that jitter is itself the signal that the
  setting is not good enough.

## What these rounds do NOT prove

**The acoustics of the real microphone.** `say` output is much cleaner than "a MEMS microphone
across a desk in a noisy room", so what is verified above is the **pipeline** (format,
compression, language, latency), not the **acoustics**. That last step needs the device side to
record: play these synthesized clips from the laptop and let the device record them — only then
is it end to end.
