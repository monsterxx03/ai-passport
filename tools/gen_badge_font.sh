#!/usr/bin/env bash
# 生成设备侧的中文字体（main/badge_font_16.c）。
#
# 为什么生成结果不进仓库：它是从**本机**的中文字体文件切出来的，而 macOS 自带的
# 中文字体（Arial Unicode、Hiragino）授权不允许再分发，且源码文本有 15MB。
# 要对外发布时，把 BADGE_FONT_SOURCE 指向一份 OFL 字体（思源黑体 / Noto Sans SC）
# 即可，脚本其余部分不用动；那时再把产物连同来源与授权一起提交。
#
# 字符范围：ASCII、CJK 标点、CJK 统一表意文字、全角形式。不逐字枚举是因为这块屏
# 要显示的是 tachi 下发的**任意**中文（会话标题、命令预览、模型提的问题），
# 而 LVGL 内置的 CJK 子集小到连「脑」「还」「连」都不含——缺一个字就是一个方框。
set -euo pipefail

FONT="${BADGE_FONT_SOURCE:-/Library/Fonts/Arial Unicode.ttf}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/main/badge_font_16.c"
RANGES="0x20-0x7F,0x3000-0x303F,0x4E00-0x9FA5,0xFF01-0xFF5E"

if [[ ! -f "${FONT}" ]]; then
    echo "找不到字体：${FONT}" >&2
    echo "用 BADGE_FONT_SOURCE=<一个含中文的 TTF/OTF> 再来一次。" >&2
    exit 1
fi

# lv_font_conv 固定版本：字体文件是构建产物，两版工具生成的字节不同会让固件
# 哈希无端变化。
#
# --no-compress 不是可选项：lv_font_conv 默认给位图做 RLE 压缩并写
# bitmap_format=1（COMPRESSED），而那个压缩格式是 LVGL 8 时代的，LVGL 9.5 的
# 解码器解不出来——表现是「字形查得到、尺寸也对，但屏幕上什么都不画」，而
# ASCII 用内置字体照常显示，很容易误判成别的问题。加上它，输出 PLAIN 位图。
npx -y lv_font_conv@1.5.2 \
    --font "${FONT}" \
    --size 16 --bpp 4 \
    --range "${RANGES}" \
    --no-compress \
    --format lvgl --lv-include lvgl.h \
    -o "${OUT}"

echo "已生成 ${OUT}（$(du -h "${OUT}" | cut -f1)）"
