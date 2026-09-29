#!/usr/bin/env bash
# 生成设备侧的字体：main/badge_font_16.c（中文）与 main/badge_font_icon_16.c（两枚传输图标）。
#
# 产物**随仓库分发**（18 MB 左右的源码文本），所以平时不需要跑这个脚本——只有换字体、
# 改字符范围、或者动了字号时才重新生成一次。
#
# 也正因为产物入库，源字体必须是**允许再分发**的：默认用 LVGL 组件里自带的那份思源黑体
# （Source Han Sans SC，SIL OFL 1.1），它跟依赖一起拉下来，不需要额外下载。系统自带的
# 中文字体（Arial Unicode、Hiragino）授权不允许再分发，**不要**用它们生成要提交的产物。
set -euo pipefail

FONT="${BADGE_FONT_SOURCE:-}"
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/main/badge_font_16.c"
if [[ -z "${FONT}" ]]; then
    FONT="${ROOT}/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
fi
# 字符范围要覆盖**设备上会出现的任意字符**，而不只是「常用汉字」：
#   - 基本拉丁：ASCII 与英文 UI
#   - 拉丁补充：`·`(U+00B7)、`°`、`×` 这些标点与人名里的重音字母
#   - 通用标点：`…`(U+2026)、`—`(U+2014)、`•`(U+2022)、弯引号——tachi 下发的
#     状态副标题（「推理中…」）和命令预览里都会出现。少了这一段，屏幕上就是
#     LVGL 的缺字占位方框，而 UTF-8 正确、构建通过，看起来像界面坏了。
#   - **符号区（箭头 / 数学 / 技术符号 / 圈号 / 几何 / 杂项 / 装饰）**：这些是
#     **模型自己写**的，没有固定清单可枚举。`⌘`(U+2318)、`⇧`、`→`、`≤`、`✓`
#     都会出现在提问与选项里——一个真实例子是「⌘=/⌘- 的冲突怎么解？」。
#   - CJK 标点 / 全角形式 / CJK 统一表意文字
#
# 范围宁宽勿窄：每多一段只是让字体文件大几十 KB，而漏一个字符就是用户看得见的
# 方框。唯一要避开的是 emoji（U+1F300 以上，体积按 MB 计）。
RANGES="0x20-0x7F,0xA0-0xFF,0x2000-0x206F,0x2190-0x24FF,0x25A0-0x27BF,0x3000-0x303F,0x4E00-0x9FA5,0xFF01-0xFF5E"

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

# ── 图标字体（main/badge_font_icon_16.c）─────────────────────────────────────
#
# 顶栏那两枚图标（USB / 蓝牙）没有任何 Unicode 码位，所以它们只能来自**图标字体**的
# 私有区：USB = U+F287、蓝牙 = U+F293，两个都是 FontAwesome 的 brands 字形。默认就用
# LVGL 组件里自带的那份 FontAwesome（`scripts/built_in_font/`，跟依赖一起拉下来，
# 不需要额外下载）；它的授权文本在同一目录的 `font_license/FontAwesome5/LICENSE.txt`。
# 要换别的图标字体就设 BADGE_ICON_FONT_SOURCE（例如本机装了 Nerd Font 的话）。
#
# 单独一份字体、而不是并进上面那份，是为了「缺了会响」：图标字体没生成时是链接期
# 找不到符号，而并进中文字体时缺字只会变成屏幕上一个方框（那正是这份文档反复警告的
# 失败方式）。代价是脚本多一次 npx 调用。
ICON_FONT="${BADGE_ICON_FONT_SOURCE:-}"
ICON_OUT="${ROOT}/main/badge_font_icon_16.c"
ICON_RANGES="0xF287,0xF293"
ICON_FONT_DEFAULT="${ROOT}/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"

if [[ -z "${ICON_FONT}" ]]; then
    ICON_FONT="${ICON_FONT_DEFAULT}"
fi

if [[ ! -f "${ICON_FONT}" ]]; then
    echo "找不到图标字体（USB / 蓝牙两枚图标在里面）：${ICON_FONT}" >&2
    echo "它本该随依赖一起拉下来——先跑一次 idf.py build，或者用" >&2
    echo "BADGE_ICON_FONT_SOURCE=<一份含 U+F287/U+F293 的 TTF/OTF/WOFF> 指定别的。" >&2
    exit 1
fi

npx -y lv_font_conv@1.5.2 \
    --font "${ICON_FONT}" \
    --size 16 --bpp 4 \
    --range "${ICON_RANGES}" \
    --no-compress \
    --format lvgl --lv-include lvgl.h \
    -o "${ICON_OUT}"

echo "已生成 ${ICON_OUT}（图标来自 ${ICON_FONT}）"
