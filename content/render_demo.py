#!/usr/bin/env python3
"""把设备上的笔顺动画逐像素还原成 GIF —— 宣传/演示用

不是示意图：像素、配色、米字格、高亮规则、时序全都照着固件里那套来，
输入就是烧进 SD 卡的那两个文件（img565/<id>.bin 和 anim/<id>.bin）。
固件那边改了 GRID_*/HL_* 或者动画节奏，这里要跟着改。

用法: python render_demo.py 永 好 学
"""
import json, struct, sys
from pathlib import Path
from PIL import Image

BASE = Path(__file__).parent
OUT  = BASE.parent / "docs/media"

SCR_W, SCR_H = 368, 448
GRID_X, GRID_Y, GRID_S = 64, 98, 240        # 和 user_app.cpp 的 GRID_* 一致
GRID_RGB = (78, 26, 26)                     # 暗红米字格
HL = (255, 190, 60)                         # 正在写的那一笔

FRAME_MS   = 60
WRITE_FR   = 10                             # 一画写 10 帧 ≈ 600 ms（跟着笔画名的长度走）
PAUSE_FR   = 10                             # 写完停 600 ms，留给孩子跟着写（STROKE_GAP_FAST_MS）
END_FR     = 25                             # 整字写完停一下再循环
SCALE      = 0.75


def load565(path):
    """SD 上是大端 RGB565，还原成 RGB888。固件 amoled_rgb() 存的时候换过字节序。"""
    raw = open(path, "rb").read()
    px = struct.unpack(f">{SCR_W * SCR_H}H", raw)
    img = [((v >> 11) & 31, (v >> 5) & 63, v & 31) for v in px]
    return [(r * 255 // 31, g * 255 // 63, b * 255 // 31) for r, g, b in img], px


def load_anim(path):
    d = open(path, "rb").read()
    assert d[:4] == b"PSA1", path
    n, w, h, _fl, x0, y0 = struct.unpack("<BBBBHH", d[4:12])
    assert (w, h, x0, y0) == (GRID_S, GRID_S, GRID_X, GRID_Y)
    cnt = struct.unpack(f"<{n}H", d[12:12 + n * 2])
    off, out = 12 + n * 2, []
    for c in cnt:
        out.append([(d[off + i * 2], d[off + i * 2 + 1]) for i in range(c)])
        off += c * 2
    return out


def tint(px565):
    """跟固件 tint565() 一个算法：按原像素亮度重新配暖黄，抗锯齿的灰边才不会描成硬边"""
    a = (px565 >> 11) & 31
    return (HL[0] * a // 31, HL[1] * a // 31, HL[2] * a // 31)


def render(ch, rec):
    rgb, px565 = load565(BASE / f"out/img565/{rec['id']:04d}.bin")
    strokes = load_anim(BASE / f"out/anim/{rec['id']:04d}.bin")

    base = Image.new("RGB", (SCR_W, SCR_H), (0, 0, 0))
    base.putdata(rgb)

    # 起手画面：字收起来，只留拼音、词组和空米字格
    canvas = base.copy()
    for y in range(GRID_Y, GRID_Y + GRID_S):
        for x in range(GRID_X, GRID_X + GRID_S):
            canvas.putpixel((x, y), (0, 0, 0))
    # 米字格：只落在黑底上 —— 所以格线永远压在字底下，静态和动画共用一次调用
    grid = []
    x1, y1 = GRID_X + GRID_S - 1, GRID_Y + GRID_S - 1
    cx, cy = GRID_X + GRID_S // 2, GRID_Y + GRID_S // 2
    for i in range(GRID_S):
        grid += [(GRID_X + i, GRID_Y), (GRID_X + i, y1), (GRID_X, GRID_Y + i), (x1, GRID_Y + i)]
        if (i // 6) & 1:                     # 中线和对角线走虚线，实 6 虚 6
            continue
        grid += [(GRID_X + i, cy), (cx, GRID_Y + i), (GRID_X + i, GRID_Y + i), (GRID_X + i, y1 - i)]
    for x, y in grid:
        if canvas.getpixel((x, y)) == (0, 0, 0):
            canvas.putpixel((x, y), GRID_RGB)

    frames = [canvas.copy()] * 6
    for st in strokes:
        done = 0
        for f in range(1, WRITE_FR + 1):
            upto = len(st) * f // WRITE_FR
            for bx, by in st[done:upto]:
                x, y = GRID_X + bx, GRID_Y + by
                canvas.putpixel((x, y), tint(px565[y * SCR_W + x]))
            done = upto
            frames.append(canvas.copy())
        for bx, by in st:                    # 这一画写完，高亮转成正式的白
            x, y = GRID_X + bx, GRID_Y + by
            canvas.putpixel((x, y), rgb[y * SCR_W + x])
        frames += [canvas.copy()] * PAUSE_FR
    frames += [canvas.copy()] * END_FR

    if SCALE != 1:
        sz = (int(SCR_W * SCALE), int(SCR_H * SCALE))
        frames = [f.resize(sz, Image.LANCZOS) for f in frames]
    out = OUT / f"stroke-{ch}.gif"
    frames[0].save(out, save_all=True, append_images=frames[1:],
                   duration=FRAME_MS, loop=0, optimize=True)
    print(f"{ch} id={rec['id']} {len(strokes)} 画 {len(frames)} 帧 "
          f"{out.stat().st_size / 1024:.0f} KB → {out}")


def main():
    idx = {r["char"]: r for r in json.load(open(BASE / "out/chars.json"))}
    for ch in sys.argv[1:]:
        render(ch, idx[ch])


if __name__ == "__main__":
    main()
