#!/usr/bin/env bash
# 跑一轮转写探针。
#
# 为什么非要打包成 .app：TCC 把「语音识别」这项授权归因给 **responsible process**，
# 而且判定发生在**第一次调用**时。裸 CLI 的 responsible process 是终端，终端没有
# NSSpeechRecognitionUsageDescription —— 结果是直接崩，不是「弹个框请求授权」。
# 所以下面这段 plist + codesign 是必需的，不是洁癖。
#
# 用法：./run.sh 1      # 轮次见 README.md
set -euo pipefail

ROUND="${1:?用法: ./run.sh <轮次，1..6，见 README.md>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/$(cd "$HERE" && ls round${ROUND}_*.swift)"
NAME="SttProbe${ROUND}"
APP="$HERE/build/${NAME}.app"
LOG="$HERE/logs/round${ROUND}.log"

mkdir -p "$APP/Contents/MacOS" "$HERE/logs"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>${NAME}</string>
  <key>CFBundleIdentifier</key><string>dev.tachi.sttprobe${ROUND}</string>
  <key>CFBundleName</key><string>SttProbe</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>26.0</string>
  <key>NSSpeechRecognitionUsageDescription</key><string>转写探针：把合成语音转成文字，验证本机识别能力</string>
  <key>NSMicrophoneUsageDescription</key><string>转写探针不用麦克风，但系统设置里这一项与语音识别挨着</string>
</dict>
</plist>
PLIST

echo "→ 编译 $SRC"
swiftc -O -parse-as-library \
  -target "$(uname -m)-apple-macosx26.0" \
  -o "$APP/Contents/MacOS/${NAME}" "$SRC"
codesign --force --sign - --identifier "dev.tachi.sttprobe${ROUND}" "$APP" >/dev/null

rm -f "$LOG"
echo "→ 运行（第一次会弹「允许语音识别」，要同意）"
# -W = 等应用退出。不用 pgrep 轮询等它起来：open 是异步的，而轮询会在
# 「进程还没出现」和「已经跑完退出」两种情况下都立刻放过去，然后 cat 一个还不存在的日志。
open -W -a "$APP"

echo "→ 日志 $LOG"
if [ -f "$LOG" ]; then
    cat "$LOG"
else
    echo "（没有日志——多半是授权被拒或崩了，看 系统设置 → 隐私与安全性 → 语音识别）"
fi
