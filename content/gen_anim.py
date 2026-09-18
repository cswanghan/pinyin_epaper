#!/usr/bin/env python3
"""生成笔顺动画数据：每个生字一份「像素按笔画分好组、组内按书写先后排好序」的表

  out/anim/<id>.bin      设备直接 fread 的二进制
  out/preview_anim/<id>_<char>.png   分帧预览（--preview 时才出）

关键取舍：动画的像素不是新画的，是从 out/img565/<id>.bin 里那个已经渲染好的大字
上原样抠下来的，这里只回答「这个像素属于第几画、在这一画的第几个位置」。
所以动画写完留在屏上的字和静态图逐像素一致 —— 不会出现写完之后字形跳一下。
抗锯齿的灰边也一并归了画，不会有像素被落下。

笔画归属来自 makemeahanzi 的轮廓和中线（graphics.txt，和 gen_stroke_order.py 同一份源）。
它是另一套楷体，部件位置和霞鹜文楷有细微出入，个别字可能有几个像素被分到相邻的一画上；
但字形本身不受影响（字形是霞鹜的），最坏也就是某个像素早一画或晚一画出现。

画数和 out/stroke/bishun.txt 逐字核对过，2500 字全部一致 —— 动画和念的笔画名不会错位。

依赖:
  curl -fL -o graphics.txt https://raw.githubusercontent.com/skishore/makemeahanzi/master/graphics.txt
  （别走 jsdelivr，它对 >20MB 的文件返回一句错误文本但退出码是 0）

用法:
  python gen_anim.py                 全量 2500 字
  python gen_anim.py --preview 你好  只出这几个字，并画分帧预览图
"""
import json, struct, sys, re
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

BASE     = Path(__file__).parent
GRAPHICS = BASE / "graphics.txt"
CHARS    = BASE / "out/chars.json"
IMG565   = BASE / "out/img565"
OUTDIR   = BASE / "out/anim"
PREVIEW  = BASE / "out/preview_anim"

SCR_W, SCR_H = 368, 448           # 整屏，和 gen_images_amoled.py 一致

# 米字格/动画框。全量扫过 2500 张图，大字墨迹落在 y 115..320 / x 79..292，
# 这个框上下左右都留了十几像素余量，又够不着拼音(底边 y=80)和分隔线(y=344)。
# 240 < 256，所以框内坐标一个字节存得下。
BOX_X, BOX_Y, BOX_W, BOX_H = 64, 98, 240, 240

MAGIC = b"PSA1"
MAX_STROKES = 48                  # 一个字节存画数；实际最多三十几画

# 中线采样步长（框内像素）。密一点归属更准，慢一点；4 px 实测足够。
MEDIAN_STEP = 4.0
# 落在某一画轮廓内的像素，到该画中线的距离打这个折 —— 让「在自己笔画里」压过「离别人中线近」
INSIDE_BONUS = 0.4


# ---------- SVG 路径 ----------
_TOK = re.compile(r"[MLQCZmlqcz]|-?\d+(?:\.\d+)?")

def flatten(path, steps=8):
    """makemeahanzi 只用 M/L/Q/C/Z，全是绝对坐标。返回若干条闭合折线。"""
    toks = _TOK.findall(path)
    subs, cur, pt, start, i = [], [], (0.0, 0.0), (0.0, 0.0), 0
    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    while i < len(toks):
        c = toks[i]; i += 1
        if c in "Mm":
            if len(cur) > 2: subs.append(cur)
            pt = start = (num(), num()); cur = [pt]
        elif c in "Ll":
            pt = (num(), num()); cur.append(pt)
        elif c in "Qq":
            cx, cy, x, y = num(), num(), num(), num()
            for s in range(1, steps + 1):
                t = s / steps; u = 1 - t
                cur.append((u*u*pt[0] + 2*u*t*cx + t*t*x, u*u*pt[1] + 2*u*t*cy + t*t*y))
            pt = (x, y)
        elif c in "Cc":
            x1, y1, x2, y2, x, y = (num() for _ in range(6))
            for s in range(1, steps + 1):
                t = s / steps; u = 1 - t
                cur.append((u**3*pt[0] + 3*u*u*t*x1 + 3*u*t*t*x2 + t**3*x,
                            u**3*pt[1] + 3*u*u*t*y1 + 3*u*t*t*y2 + t**3*y))
            pt = (x, y)
        elif c in "Zz":
            if len(cur) > 2: subs.append(cur)
            cur = [start]; pt = start
    if len(cur) > 2: subs.append(cur)
    return subs


def densify(pts, step):
    """把中线折线按固定步长重采样，同时返回每个采样点的弧长比例 0~1。"""
    p = np.asarray(pts, dtype=np.float64)
    if len(p) == 1:
        return p, np.zeros(1)
    seg = np.hypot(*(p[1:] - p[:-1]).T)
    cum = np.concatenate([[0.0], np.cumsum(seg)])
    total = cum[-1]
    if total <= 0:
        return p[:1], np.zeros(1)
    n = max(2, int(total / step) + 1)
    s = np.linspace(0, total, n)
    return np.stack([np.interp(s, cum, p[:, 0]), np.interp(s, cum, p[:, 1])], 1), s / total


# ---------- 主流程 ----------
def load_graphics(wanted):
    g = {}
    with open(GRAPHICS, encoding="utf-8") as f:
        for ln in f:
            d = json.loads(ln)
            if d["character"] in wanted:
                g[d["character"]] = d
    return g


def build(rec, glyph):
    """返回 (每画的像素表, 提示串)。像素表是 list[np.ndarray(n,2) uint8]，框内坐标 (x,y)。"""
    a = np.fromfile(IMG565 / f"{rec['id']:04d}.bin", dtype=">u2").reshape(SCR_H, SCR_W)
    box = a[BOX_Y:BOX_Y + BOX_H, BOX_X:BOX_X + BOX_W]
    iy, ix = np.nonzero(box)
    if len(iy) == 0:
        return None, "图里没墨"

    # makemeahanzi 的 y 轴朝上（SVG 里配的是 scale(1,-1) translate(0,-900)），
    # 直接拿来用会把笔顺上下颠倒 —— 「三」会从下往上写。取负翻过来，
    # 具体平移交给下面按外接框对齐那步，不用关心它那个 900 的偏移。
    flip = lambda pts: [(x, -y) for x, y in pts]
    strokes = [[flip(sub) for sub in flatten(s)] for s in glyph["strokes"]]
    medians = [flip(m) for m in glyph["medians"]]
    n = len(strokes)

    # em 坐标 → 框内坐标。按各自的外接框对齐（两套字体的字面框约定不一样，
    # 直接套 1024 的 em 盒会偏），x/y 独立缩放。
    allp = np.concatenate([np.asarray(p) for s in strokes for p in s])
    ex0, ey0 = allp.min(0); ex1, ey1 = allp.max(0)
    gx0, gx1, gy0, gy1 = ix.min(), ix.max(), iy.min(), iy.max()
    sx = (gx1 - gx0) / max(ex1 - ex0, 1e-6)
    sy = (gy1 - gy0) / max(ey1 - ey0, 1e-6)
    def to_box(p):
        p = np.asarray(p, dtype=np.float64)
        return np.stack([(p[:, 0] - ex0) * sx + gx0, (p[:, 1] - ey0) * sy + gy0], 1)

    ink = np.stack([ix, iy], 1).astype(np.float32)          # (N,2)
    best = np.full(len(ink), np.inf, dtype=np.float32)
    owner = np.zeros(len(ink), dtype=np.int16)
    tpos = np.zeros(len(ink), dtype=np.float32)

    for k in range(n):
        # 这一画的轮廓 → 掩膜
        m = Image.new("1", (BOX_W, BOX_H), 0)
        dr = ImageDraw.Draw(m)
        for sub in strokes[k]:
            dr.polygon([tuple(q) for q in to_box(sub)], fill=1)
        inside = np.asarray(m)[ink[:, 1].astype(int), ink[:, 0].astype(int)]

        mp, ms = densify(to_box(medians[k]), MEDIAN_STEP)
        d = np.sqrt(((ink[:, None, :] - mp[None, :, :].astype(np.float32)) ** 2).sum(-1))
        j = d.argmin(1)
        dist = d[np.arange(len(ink)), j].astype(np.float32)
        score = np.where(inside, dist * INSIDE_BONUS, dist)

        hit = score < best
        best[hit] = score[hit]; owner[hit] = k; tpos[hit] = ms[j][hit]

    out, orphan = [], 0
    for k in range(n):
        sel = np.nonzero(owner == k)[0]
        if len(sel) == 0:
            orphan += 1
            out.append(np.zeros((0, 2), dtype=np.uint8))
            continue
        sel = sel[np.argsort(tpos[sel], kind="stable")]
        out.append(np.stack([ix[sel], iy[sel]], 1).astype(np.uint8))
    return out, (f"{orphan} 画没分到像素" if orphan else "")


def pack(strokes):
    n = len(strokes)
    hdr = MAGIC + struct.pack("<BBBBHH", n, BOX_W, BOX_H, 0, BOX_X, BOX_Y)
    hdr += struct.pack(f"<{n}H", *[len(s) for s in strokes])
    return hdr + b"".join(s.tobytes() for s in strokes)


def preview(rec, strokes, path):
    """分帧图：每画一格，画到第 k 画为止，当前画高亮。"""
    n = len(strokes)
    cols = min(n, 6); rows = (n + cols - 1) // cols
    cell = 120
    sheet = Image.new("RGB", (cols * cell, rows * cell), (0, 0, 0))
    acc = Image.new("RGB", (BOX_W, BOX_H), (0, 0, 0))
    px = acc.load()
    for k in range(n):
        for x, y in strokes[k]:
            px[int(x), int(y)] = (255, 190, 60)
        frame = acc.resize((cell, cell), Image.LANCZOS)
        sheet.paste(frame, ((k % cols) * cell, (k // cols) * cell))
        for x, y in strokes[k]:
            px[int(x), int(y)] = (255, 255, 255)
    sheet.save(path)


def main():
    argv = sys.argv[1:]
    prev = "--preview" in argv
    if prev: argv.remove("--preview")

    recs = json.loads(CHARS.read_text(encoding="utf-8"))
    if argv:
        want = set("".join(argv))
        recs = [r for r in recs if r["char"] in want]
    if not recs:
        sys.exit("没有要生成的字")

    g = load_graphics({r["char"] for r in recs})
    OUTDIR.mkdir(parents=True, exist_ok=True)
    if prev: PREVIEW.mkdir(parents=True, exist_ok=True)

    total = skipped = 0
    for i, r in enumerate(recs, 1):
        glyph = g.get(r["char"])
        if not glyph or len(glyph["strokes"]) > MAX_STROKES:
            skipped += 1
            print(f"  跳过 {r['char']}（没有轮廓数据）")
            continue
        strokes, note = build(r, glyph)
        if strokes is None:
            skipped += 1; print(f"  跳过 {r['char']}：{note}"); continue
        if note: print(f"  {r['char']}: {note}")
        (OUTDIR / f"{r['id']:04d}.bin").write_bytes(pack(strokes))
        total += 1
        if prev: preview(r, strokes, PREVIEW / f"{r['id']:04d}_{r['char']}.png")
        if not prev and (i % 100 == 0 or i == len(recs)):
            print(f"  {i}/{len(recs)}", flush=True)

    sz = sum(f.stat().st_size for f in OUTDIR.glob("*.bin"))
    print(f"完成 {total} 字（跳过 {skipped}）→ {OUTDIR}，共 {sz/1024/1024:.0f} MB")


if __name__ == "__main__":
    main()
