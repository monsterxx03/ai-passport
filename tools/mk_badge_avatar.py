# /// script
# dependencies = ["pillow", "numpy"]
# ///
"""把 tachi 的 app 图标转成设备侧能画的 LVGL 图片（RGB565）。

    uv run tools/mk_badge_avatar.py

依赖写在下面的 script 块里，uv 会自己准备环境——本机不需要预装 pillow/numpy。
"""
import os
from PIL import Image, ImageDraw
import sys
import pathlib

# 源图在 tachi 仓库里；换头像时改这个路径即可（默认按两个仓库并排摆放）。
SRC = os.environ.get("BADGE_AVATAR_SRC",
                    os.path.expanduser("~/repos/tachi/desktop/build/appicon.png"))
OUT = str(pathlib.Path(__file__).resolve().parents[1] / "main" / "badge_avatar.c")
SIZE = 120          # 屏幕上那块圆的直径。84 时细节全糊，120 还能认出来是谁

img = Image.open(SRC).convert("RGBA")

# 1) 裁掉白边：图案本身是圆的，四周是白底。找非白像素的边界框。
#    留一点余量（图案本身的圆角），否则会切到最外圈的描边。
gray = img.convert("L")
bbox = gray.point(lambda v: 255 if v < 240 else 0).getbbox()
if bbox:
    img = img.crop(bbox)
print("裁白边后:", img.size)

# 2) 缩放到目标尺寸，正方形
img = img.resize((SIZE, SIZE), Image.LANCZOS)

# 3) 把外围的「底」换成屏幕底色。
#
#    源图有两层底：最外面是**透明**（转 RGB 后是黑），再往里是图案自己画的
#    一块**白色圆角方块**——机器人就画在它上面，所以只裁四个角是不够的，
#    屏幕上会留下一块刺眼的白方块，看起来仍然不是「一个圆形头像」。
#
#    做法是从边框向内做一次连通搜索（BFS），把**与边框连通**的底色像素都换成
#    屏幕色。之所以要「连通」而不是「颜色接近就换」：图案内部的白（机体、高光、
#    锯齿）与边框不连通，按颜色一刀切会把它们一起挖掉，机器人会变成镂空的。
import numpy as np
from collections import deque

BG = (15, 19, 25)   # 与状态屏的底色一致
arr = np.array(img.convert("RGB"))
height, width, _ = arr.shape


def is_backdrop(pixel):
    """底色：接近黑（透明被拍平）或接近白（那块白底）。

    阈值要够宽，否则会在**圆角处的抗锯齿过渡**（灰色，既不像白也不像黑）前停下，
    于是图标四个角各留一个白点——它们与边框不连通，BFS 走不过去。放宽之后
    图案内部的白仍然安全：那块白被描边围着，从边框到不了。
    """
    r, g, b = int(pixel[0]), int(pixel[1]), int(pixel[2])
    return (r < 45 and g < 45 and b < 45) or (r > 228 and g > 228 and b > 228)


backdrop = np.zeros((height, width), dtype=bool)
queue = deque()
for x in range(width):
    for y in (0, height - 1):
        if is_backdrop(arr[y, x]):
            backdrop[y, x] = True
            queue.append((y, x))
for y in range(height):
    for x in (0, width - 1):
        if is_backdrop(arr[y, x]) and not backdrop[y, x]:
            backdrop[y, x] = True
            queue.append((y, x))

while queue:
    y, x = queue.popleft()
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        ny, nx = y + dy, x + dx
        if 0 <= ny < height and 0 <= nx < width and not backdrop[ny, nx] \
                and is_backdrop(arr[ny, nx]):
            backdrop[ny, nx] = True
            queue.append((ny, nx))

arr[backdrop] = BG
print("底色像素:", int(backdrop.sum()), f"({backdrop.sum() / (height * width):.0%})")

# 4) 圆外涂成底色，并且**圆收小 8 像素**。
#
#    为什么不是内切圆：源图那圈白底是个圆角方形，它的四个角在圆角处的抗锯齿是灰的，
#    BFS 走到那里会停下，于是四个角各留一个白点（放宽阈值则会把图案自己的浅色
#    一起吃成虚线——试过，更糟）。图案是居中构图的，收掉边缘这 8 像素伤不到它，
#    而四个角点恰好都落在里面。
INSET = 8
mask = Image.new("L", (width, height), 0)
ImageDraw.Draw(mask).ellipse((INSET, INSET, width - 1 - INSET, height - 1 - INSET), fill=255)
img = Image.composite(Image.fromarray(arr), Image.new("RGB", (width, height), BG), mask)

# 5) LVGL 的 RGB565：小端
pixels = img.load()
data = bytearray()
for y in range(SIZE):
    for x in range(SIZE):
        r, g, b = pixels[x, y]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.append(v & 0xFF)
        data.append((v >> 8) & 0xFF)

with open(OUT, "w") as f:
    f.write("// 由 desktop 的 app 图标生成（tools/mk_badge_avatar.py）。\n")
    f.write("// 这是构建产物：源图在 tachi 仓库里，改动它之后要重新生成。\n")
    f.write('#include "lvgl.h"\n\n')
    f.write(f"#define BADGE_AVATAR_SIZE {SIZE}\n\n")
    f.write("static const uint8_t badge_avatar_map[] = {\n")
    for i in range(0, len(data), 16):
        f.write("    " + ", ".join(f"0x{b:02x}" for b in data[i:i+16]) + ",\n")
    f.write("};\n\n")
    f.write("const lv_image_dsc_t badge_avatar = {\n")
    # magic 不能省：LVGL 用它判断「这是我认识的内置图片数据」。缺了它，解码器会
    # 把这段数据当成文件路径去找，失败之后整屏什么都画不出来（表现为白屏），
    # 而设备本身还在正常收发协议——很容易误判成内存或渲染的问题。
    f.write("    .header.magic = LV_IMAGE_HEADER_MAGIC,\n")
    f.write("    .header.cf = LV_COLOR_FORMAT_RGB565,\n")
    f.write(f"    .header.w = {SIZE},\n")
    f.write(f"    .header.h = {SIZE},\n")
    f.write(f"    .header.stride = {SIZE * 2},\n")
    f.write("    .data_size = sizeof(badge_avatar_map),\n")
    f.write("    .data = badge_avatar_map,\n")
    f.write("};\n")
print("已写出", OUT, f"{len(data)} 字节像素")
