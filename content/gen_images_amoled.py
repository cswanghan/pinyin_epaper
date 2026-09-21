#!/usr/bin/env python3
"""生成 AMOLED 彩屏图片：每个生字一张 368x448 RGB565 图

输出两份:
  out/preview565/<id>_<char>.png   PNG 预览（给人看）
  out/img565/<id>.bin              设备原生格式，329728 字节

墨水屏那套 200x200 四色图归 gen_images.py 管，两边互不影响 ——
一张 SD 卡要同时喂两块板子，所以目录也分开：img/ 给墨水屏，img565/ 给 AMOLED。

设备格式:
  RGB565 大端（高字节在前），从左上角逐行铺满，没有文件头。
  这个字节序就是 amoled_port.h 里 amoled_rgb() 在 ESP32-S3 上落到内存的样子：
  它返回前把 uint16 做了字节交换，小端存储之后内存里正好是「高字节在前」。
  固件直接 fread 进整屏缓冲再 panel_push，不做任何转换。

配色走「纯黑底」：AMOLED 的黑像素是不发光的，一个字可能在屏上停几分钟，
白底既费电又有烧屏风险。
"""
import json, sys, unicodedata
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

BASE = Path(__file__).parent
FONT = BASE / "fonts/LXGWWenKai-Regular.ttf"     # 霞鹜文楷：楷体，小学课本字形
CHARS = BASE / "out/chars.json"
PREVIEW = BASE / "out/preview565"
IMGDIR  = BASE / "out/img565"

W, H = 368, 448
FB_BYTES = W * H * 2                              # 329728，对上 AMOLED_FB_BYTES

# ---- 配色 ----
BG   = (0, 0, 0)
CHAR_C = (255, 255, 255)
WORD_C = (165, 165, 165)
SEP_C  = (70, 70, 70)
# 四声四色，轻声灰。多音字每个音节按自己的调上色
TONE_C = {1: (255, 90, 90), 2: (255, 180, 40), 3: (90, 210, 110),
          4: (90, 175, 255), 0: (170, 170, 170)}
TONE_MARK = {'̄': 1, '́': 2, '̌': 3, '̀': 4}

# ---- 版式（y 坐标）----
SIDE                  = 18
PINYIN_TOP, PINYIN_SZ = 18, 56
PINYIN_MAX_W          = 280      # 比正文窄，给右上角「已掌握」勾让出地方
CHAR_CY, CHAR_SZ      = 218, 216
SEP_Y                 = 344
WORDS_TOP, WORD_SZ, LINE_H = 358, 34, 42
WORD_LINES            = 2

# 右上角留给「已掌握」红勾的空白框，固件运行时往这儿画（见 user_app.cpp draw_check）。
# 这里不画，只是保证版式不会占用它。
MASTER_BOX = (316, 14, 356, 54)

# 左上角留给电量图标，同样是固件画的（user_app.cpp draw_battery565）。
# 现在靠 PINYIN_MAX_W=280 居中自然让出了 x<44 这一条；写在这儿是为了将来有人
# 调宽拼音或往左上角加东西时，知道这块地方被占着。
BATTERY_BOX = (4, 20, 39, 36)


def tone_of(syl):
    for ch in unicodedata.normalize("NFD", syl):
        if ch in TONE_MARK:
            return TONE_MARK[ch]
    return 0


def fit(draw, text, size, max_w):
    """字号自适应：太宽就缩，直到放得下。"""
    while size > 10:
        f = ImageFont.truetype(str(FONT), size)
        if draw.textlength(text, font=f) <= max_w:
            return f
        size -= 2
    return ImageFont.truetype(str(FONT), 10)


def wrap_words(draw, words, font, max_w):
    """词组贪心排版，两个空格分隔，最多 WORD_LINES 行。"""
    lines, cur = [], ""
    for w in words:
        t = w if not cur else cur + "  " + w
        if draw.textlength(t, font=font) <= max_w:
            cur = t
        else:
            if cur:
                lines.append(cur)
                if len(lines) == WORD_LINES:
                    return lines
            cur = w
    if cur and len(lines) < WORD_LINES:
        lines.append(cur)
    return lines


def render(rec):
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    # 拼音：多音字并排，各按自己的声调上色
    syls = rec["readings"] or [rec["pinyin"]]
    joined = "  ".join(syls)
    f_py = fit(d, joined, PINYIN_SZ, PINYIN_MAX_W)
    gap = d.textlength("  ", font=f_py)
    x = (W - d.textlength(joined, font=f_py)) / 2
    for i, s in enumerate(syls):
        d.text((x, PINYIN_TOP), s, font=f_py, fill=TONE_C[tone_of(s)])
        x += d.textlength(s, font=f_py) + (gap if i < len(syls) - 1 else 0)

    # 大字：按 bbox 居中，不按基线 —— 不然「一」会贴到上面去
    f_ch = ImageFont.truetype(str(FONT), CHAR_SZ)
    bb = d.textbbox((0, 0), rec["char"], font=f_ch)
    d.text(((W - (bb[2] - bb[0])) / 2 - bb[0], CHAR_CY - (bb[3] - bb[1]) / 2 - bb[1]),
           rec["char"], font=f_ch, fill=CHAR_C)

    d.rectangle([SIDE + 40, SEP_Y, W - SIDE - 40, SEP_Y + 2], fill=SEP_C)

    f_wd = ImageFont.truetype(str(FONT), WORD_SZ)
    for i, line in enumerate(wrap_words(d, rec["words"], f_wd, W - 2 * SIDE)):
        d.text(((W - d.textlength(line, font=f_wd)) / 2, WORDS_TOP + i * LINE_H),
               line, font=f_wd, fill=WORD_C)

    return img


def pack(img):
    """RGB888 → RGB565 大端字节流。"""
    a = np.asarray(img, dtype=np.uint16)
    v = ((a[:, :, 0] >> 3) << 11) | ((a[:, :, 1] >> 2) << 5) | (a[:, :, 2] >> 3)
    return v.astype(">u2").tobytes()


def main():
    recs = json.loads(CHARS.read_text(encoding="utf-8"))
    if len(sys.argv) > 1:                      # 给几个字就只出几张，用来看版式
        want = set("".join(sys.argv[1:]))
        recs = [r for r in recs if r["char"] in want]

    PREVIEW.mkdir(parents=True, exist_ok=True)
    IMGDIR.mkdir(parents=True, exist_ok=True)

    for n, rec in enumerate(recs, 1):
        img = render(rec)
        img.save(PREVIEW / f"{rec['id']:04d}_{rec['char']}.png")
        buf = pack(img)
        assert len(buf) == FB_BYTES, f"{rec['char']} 出来 {len(buf)} 字节，应为 {FB_BYTES}"
        (IMGDIR / f"{rec['id']:04d}.bin").write_bytes(buf)
        if n % 100 == 0 or n == len(recs):
            print(f"  {n}/{len(recs)}", flush=True)

    print(f"完成 {len(recs)} 张 → {IMGDIR}（每张 {FB_BYTES} 字节，"
          f"共 {len(recs) * FB_BYTES / 1024 / 1024:.0f} MB）")


if __name__ == "__main__":
    main()
