#!/usr/bin/env python3
"""生成墨水屏图片：每个生字一张 200x200 四色图

输出两份:
  out/preview/<id>_<char>.png   PNG 预览（给人看）
  out/img/<id>.bin              设备原生格式，10000 字节

设备格式（取自官方驱动 GUI_Paint.c 的 Scale==4 分支）:
  4 像素/字节，2 bit/像素，高位在前
  Addr = X/4 + Y*50 ; 位偏移 = (X%4)*2
  颜色码: 0=黑 1=白 2=黄 3=红
"""
import json, sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).parent
FONT = BASE / "fonts/LXGWWenKai-Regular.ttf"     # 霞鹜文楷：楷体，小学课本字形
CHARS = BASE / "out/chars.json"
PREVIEW = BASE / "out/preview"
IMGDIR  = BASE / "out/img"

W = H = 200
# 四色码
BLACK, WHITE, YELLOW, RED = 0, 1, 2, 3
# PNG 预览用的 RGB（贴近实际墨水屏观感）
PALETTE = {BLACK: (0, 0, 0), WHITE: (255, 255, 255),
           YELLOW: (240, 200, 40), RED: (200, 40, 40)}

# 版式（y 坐标）
PINYIN_Y, PINYIN_SZ = 6,  30
CHAR_SZ             = 104
WORD_SZ             = 21
WORDS_TOP           = 156


def fit(draw, text, font_path, size, max_w):
    """字号自适应：太宽就缩，直到放得下。"""
    while size > 8:
        f = ImageFont.truetype(str(font_path), size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 1
    return ImageFont.truetype(str(font_path), 8)


def center(draw, text, font, y, fill, img):
    w = draw.textlength(text, font=font)
    draw.text(((W - w) / 2, y), text, font=font, fill=fill)


def render(rec) -> Image.Image:
    """画一张 200x200 的 P 模式图（像素值就是四色码）。"""
    img = Image.new("P", (W, H), WHITE)
    # 调色板：索引 0-3 对应四色，便于直接存 PNG 预览
    pal = []
    for i in range(256):
        pal += list(PALETTE.get(i, (255, 255, 255)))
    img.putpalette(pal)
    d = ImageDraw.Draw(img)

    # 拼音（红色，醒目且与汉字区分）
    py = rec["pinyin"]
    if len(rec.get("readings", [])) > 1:          # 多音字标出全部读音
        py = " ".join(rec["readings"])
    f = fit(d, py, FONT, PINYIN_SZ, W - 12)
    center(d, py, f, PINYIN_Y, RED, img)

    # 大字（黑色，最高对比度，便于照着认/写）
    fc = ImageFont.truetype(str(FONT), CHAR_SZ)
    bbox = d.textbbox((0, 0), rec["char"], font=fc)
    cw, ch = bbox[2] - bbox[0], bbox[3] - bbox[1]
    d.text(((W - cw) / 2 - bbox[0], 44 - bbox[1] + (104 - ch) / 2),
           rec["char"], font=fc, fill=BLACK)

    # 黄色分隔线（用上第四种颜色，视觉分区）
    d.rectangle([16, WORDS_TOP - 6, W - 16, WORDS_TOP - 4], fill=YELLOW)

    # 词组：最多两行，每行尽量塞满
    words, lines, cur = rec["words"], [], ""
    fw = ImageFont.truetype(str(FONT), WORD_SZ)
    for w in words:
        trial = (cur + "  " + w) if cur else w
        if d.textlength(trial, font=fw) <= W - 12:
            cur = trial
        else:
            if cur: lines.append(cur)
            cur = w
        if len(lines) == 2: break
    if cur and len(lines) < 2:
        lines.append(cur)
    for i, ln in enumerate(lines[:2]):
        f2 = fit(d, ln, FONT, WORD_SZ, W - 12)
        center(d, ln, f2, WORDS_TOP + i * (WORD_SZ + 2), BLACK, img)
    return img


def pack(img: Image.Image) -> bytes:
    """转成设备原生缓冲：4 像素/字节，高位在前。"""
    px = img.load()
    out = bytearray(W // 4 * H)
    for y in range(H):
        base = y * (W // 4)
        for x in range(W):
            c = px[x, y] & 0x3
            out[base + x // 4] |= (c << 6) >> ((x % 4) * 2)
    return bytes(out)


def main():
    if not FONT.exists():
        sys.exit(f"缺少字体: {FONT}")
    recs = json.load(open(CHARS, encoding="utf-8"))
    only = sys.argv[1:] or None          # 可传指定汉字，只生成样张
    if only:
        recs = [r for r in recs if r["char"] in only]
    PREVIEW.mkdir(parents=True, exist_ok=True)
    IMGDIR.mkdir(parents=True, exist_ok=True)

    for i, r in enumerate(recs):
        img = render(r)
        img.convert("RGB").resize((400, 400), Image.NEAREST).save(
            PREVIEW / f"{r['id']:04d}_{r['char']}.png")
        (IMGDIR / f"{r['id']:04d}.bin").write_bytes(pack(img))
        if not only and (i + 1) % 250 == 0:
            print(f"  {i+1}/{len(recs)}")
    print(f"完成 {len(recs)} 张 → {IMGDIR}  预览 → {PREVIEW}")

if __name__ == "__main__":
    main()
