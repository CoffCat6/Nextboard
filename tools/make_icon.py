#!/usr/bin/env python3
"""生成 Nextboard 应用图标(app/resources/nextboard.ico)。

图标设计 = app/panel/qml/AppMark.qml 的品牌标记:
圆角方板(浅色主题 accent #3B82F6)+ 三条白色"记录"横线。

为什么要脚本而不是手绘文件:
- 图标需要多尺寸(16/20/24/32/40/48/64/128/256),手绘无法保证各尺寸一致;
- 纯标准库实现(不依赖 Pillow),4x 超采样抗锯齿 + 手写 ICO 编码,
  任何机器上都能重新生成。

⚠️ 改了 AppMark.qml 的几何/配色后,同步改下面的常量并重新运行:
    python tools/make_icon.py
"""

import math
import os
import struct

# ---- 与 AppMark.qml / Theme.qml 保持一致的常量 ----
PLATE_RGB = (0x3B, 0x82, 0xF6)        # Theme.colors.accent(浅色主题)
BAR_RGB = (0xFF, 0xFF, 0xFF)          # Theme.colors.accentText
BAR_ALPHA = (0.90, 1.00, 0.75)        # AppMark.qml 三条横线的 opacity
BAR_WIDTH_RATIO = (10 / 22, 12 / 22, 7 / 22)   # 相对板宽
BAR_HEIGHT_RATIO = 2 / 22
BAR_GAP_RATIO = 2.5 / 22
PLATE_RADIUS_RATIO = 6 / 22           # 圆角半径 / 板边长
CANVAS_PADDING = 0.05                 # 画布四周留白比例

SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
OUT_ICO = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "app", "resources", "nextboard.ico")
# 预览图不放仓库,仅用于生成后人工核对观感
OUT_PNG = os.path.join(os.environ.get("TEMP", "/tmp"), "nextboard_icon_preview.png")


def blend(fg, bg, alpha):
    return tuple(int(round(f * alpha + b * (1.0 - alpha))) for f, b in zip(fg, bg))


def fill_round_rect(buf, width, rect, radius, rgb):
    """在 RGBA 缓冲(自顶向下、不透明色)上填充一个圆角矩形。

    只做硬边填充 —— 抗锯齿由超采样后的降采样阶段负责,这样每行可用
    bytearray 切片一次性写入,避免逐像素 Python 循环。
    """
    x0, y0, x1, y1 = rect
    r = max(0.0, min(radius, (x1 - x0) / 2.0, (y1 - y0) / 2.0))
    cy0, cy1 = y0 + r, y1 - r
    pixel = bytes((rgb[0], rgb[1], rgb[2], 255))
    row_bytes = width * 4

    for py in range(max(0, int(y0)), min(width, int(math.ceil(y1)))):
        yc = py + 0.5
        if yc < cy0:
            dy = cy0 - yc
            if dy > r:
                continue
            dx = math.sqrt(max(0.0, r * r - dy * dy))
            xa, xb = x0 + r - dx, x1 - r + dx
        elif yc > cy1:
            dy = yc - cy1
            if dy > r:
                continue
            dx = math.sqrt(max(0.0, r * r - dy * dy))
            xa, xb = x0 + r - dx, x1 - r + dx
        else:
            xa, xb = x0, x1

        ia = max(0, int(math.ceil(xa)))
        ib = min(width, int(math.floor(xb)) + 1)
        if ib <= ia:
            continue
        start = py * row_bytes + ia * 4
        buf[start:start + (ib - ia) * 4] = pixel * (ib - ia)


def render(size):
    """渲染指定尺寸的 RGBA 像素(自顶向下,已抗锯齿)。"""
    ss = 4 if size <= 48 else 2          # 大尺寸像素够多,2x 即可
    w = size * ss

    buf = bytearray(w * w * 4)           # 全透明
    pad = w * CANVAS_PADDING
    plate = (pad, pad, w - pad, w - pad)
    plate_size = w - 2 * pad

    # 板
    fill_round_rect(buf, w, plate, PLATE_RADIUS_RATIO * plate_size, PLATE_RGB)

    # 三条横线(相对板尺寸,与 AppMark 的 unit 口径一致)
    unit = plate_size / 22.0
    height = max(2 * unit, 1.0 * ss)     # 保证降采样后至少 1px
    gap = 2.5 * unit
    widths = [r * 22 * unit for r in BAR_WIDTH_RATIO]
    total = 3 * height + 2 * gap
    top = w / 2.0 - total / 2.0

    for i, (bw, alpha) in enumerate(zip(widths, BAR_ALPHA)):
        y = top + i * (height + gap)
        x = w / 2.0 - bw / 2.0
        fill_round_rect(buf, w, (x, y, x + bw, y + height), height / 2.0,
                        blend(BAR_RGB, PLATE_RGB, alpha))

    # ---- 降采样(在预乘空间求平均,避免边缘出现暗边) ----
    out = bytearray(size * size * 4)
    inv = 1.0 / (ss * ss)
    for oy in range(size):
        for ox in range(size):
            pr = pg = pb = pa = 0.0
            for sy in range(ss):
                base = ((oy * ss + sy) * w + ox * ss) * 4
                for sx in range(ss):
                    o = base + sx * 4
                    a = buf[o + 3]
                    if a:
                        pr += buf[o] * a
                        pg += buf[o + 1] * a
                        pb += buf[o + 2] * a
                        pa += a
            o = (oy * size + ox) * 4
            if pa > 0:
                out[o] = int(round(pr / pa))
                out[o + 1] = int(round(pg / pa))
                out[o + 2] = int(round(pb / pa))
                out[o + 3] = int(round(pa * inv))
    return out


def encode_ico(images):
    """images: [(size, rgba_top_down)] -> ICO 字节。条目为 32 位 BGRA DIB。"""
    entries, blobs = [], b""
    offset = 6 + 16 * len(images)
    for size, rgba in images:
        # BITMAPINFOHEADER:高度写两倍(异或位图 + AND 掩码)
        header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                             size * size * 4, 0, 0, 0, 0)
        xor = bytearray()
        for y in range(size - 1, -1, -1):          # DIB 自下而上
            row = rgba[y * size * 4:(y + 1) * size * 4]
            for x in range(size):
                o = x * 4
                xor += bytes((row[o + 2], row[o + 1], row[o], row[o + 3]))  # -> BGRA
        and_stride = ((size + 31) // 32) * 4
        and_mask = bytes(and_stride * size)         # 32bpp 图标用 alpha,掩码全 0
        blob = header + bytes(xor) + and_mask
        entries.append((size, len(blob), offset))
        blobs += blob
        offset += len(blob)

    out = struct.pack("<HHH", 0, 1, len(images))
    for size, length, off in entries:
        out += struct.pack("<BBBBHHII", size if size < 256 else 0,
                           size if size < 256 else 0, 0, 0, 1, 32, length, off)
    return out + blobs


def encode_png(size, rgba):
    """极简 PNG 编码(仅用于人工核对图标效果)。"""
    import zlib
    raw = bytearray()
    for y in range(size):
        raw.append(0)                              # filter: none
        raw += rgba[y * size * 4:(y + 1) * size * 4]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def main():
    images = [(s, render(s)) for s in SIZES]
    ico = encode_ico(images)

    os.makedirs(os.path.dirname(OUT_ICO), exist_ok=True)
    with open(OUT_ICO, "wb") as f:
        f.write(ico)
    with open(OUT_PNG, "wb") as f:
        f.write(encode_png(256, dict(images)[256]))

    print(f"wrote {os.path.normpath(OUT_ICO)}  ({len(ico)} bytes, {len(SIZES)} sizes)")
    print(f"wrote {os.path.normpath(OUT_PNG)}  (256px preview)")


if __name__ == "__main__":
    main()
