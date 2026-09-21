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

import cv2
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
CROSS_SLACK  = 1.6      # 交叉判定：中线距离在最近者的这个倍数以内才算「真的压在一起」
CROSS_PAD    = 2.0
# 逐画配准：makemeahanzi 和霞鹜文楷是两套字，整字外框对齐后单个笔画仍能差七八像素，
# 所以每一画再单独平移一次去贴真墨迹。搜索半径 8 px，位移越大罚得越多。
REG_RANGE    = 8
REG_PENALTY  = 0.15
# 补断：后写的画从先写的一画身上挖走、且其实更贴先写那条中线的像素，还回去
UNCUT_FAC    = 1.3
UNCUT_PAD    = 2.0
UNCUT_PASSES = 3
REPAIR_PASSES = 6       # 浮块归位迭代轮数
# 整块认领：没有中线经过的墨块，拿各画轮廓去套，重心对齐后再上下左右找这么多像素
ORPHAN_RANGE = 6
ORPHAN_DECAY = 40.0     # 中线离这块墨每远这么多像素，认领分打一次 1/e 的折


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


K8 = np.ones((3, 3), np.uint8)


def register(inkm, masks, meds):
    """逐画平移配准：把每一画的轮廓推到最贴真墨迹的位置，中线跟着一起挪。

    整字按外接框对齐之后，makemeahanzi 的单个笔画和霞鹜文楷的墨迹仍能差七八像素
    （左右结构的两半各偏各的）。差这么多的时候「离哪条中线近」就会判错 ——
    第 2 画的中线压到了长横的左端，那一小块就被判给第 2 画，写第 2 画时凭空冒出来。
    """
    R = REG_RANGE
    pad = np.zeros((BOX_H + 2 * R, BOX_W + 2 * R), dtype=bool)
    pad[R:R + BOX_H, R:R + BOX_W] = inkm
    out_m, out_d = [], []
    for mk, (mp, ms) in zip(masks, meds):
        area = max(int(mk.sum()), 1)
        best, cx, cy = (-1e9, 0, 0), 0, 0
        for step in (2, 1):                                  # 先粗后细
            rr = R if step == 2 else 2
            for dy in range(cy - rr, cy + rr + 1, step):
                for dx in range(cx - rr, cx + rr + 1, step):
                    if abs(dx) > R or abs(dy) > R:
                        continue
                    ov = (mk & pad[R + dy:R + dy + BOX_H, R + dx:R + dx + BOX_W]).sum()
                    sc = ov / area - REG_PENALTY * ((dx * dx + dy * dy) / (R * R))
                    if sc > best[0]:
                        best = (sc, dx, dy)
            cx, cy = best[1], best[2]
        dx, dy = best[1], best[2]
        mm = np.zeros_like(mk)
        mm[max(0, dy):BOX_H + min(0, dy), max(0, dx):BOX_W + min(0, dx)] = \
            mk[max(0, -dy):BOX_H + min(0, -dy), max(0, -dx):BOX_W + min(0, -dx)]
        out_m.append(mm)
        out_d.append((mp + np.array([dx, dy], dtype=np.float64), ms))
    return out_m, out_d


def uncut(grid, dist, key, n, lockm):
    """后写的画不许把先写的一画挖断。

    一画被挖断，写它的时候中间就是一段黑的，看着像没写全；把挖走的、
    其实更贴先写那条中线的像素还给它。还回去之后后写的那一画路过这儿时
    像素已经是白的，看起来就是自然地压过去。
    """
    for _ in range(UNCUT_PASSES):
        moved = 0
        for k in range(n):
            mk = (grid == k).astype(np.uint8)
            if not mk.any():
                continue
            ncomp, lab = cv2.connectedComponents(mk, connectivity=8)
            if ncomp <= 2:
                continue
            blob = ((grid == k) | (grid > k)).astype(np.uint8)
            _, lb = cv2.connectedComponents(blob, connectivity=8)
            groups = {}
            for c in range(1, ncomp):
                groups.setdefault(int(lb[lab == c][0]), []).append(c)
            for tag, cs in groups.items():
                if len(cs) < 2:                              # 没跟别的块连在一块，不是被挖断
                    continue
                sel = (lb == tag) & (grid > k) & ~lockm
                if not sel.any():
                    continue
                sy, sx = np.nonzero(sel)
                idx = np.searchsorted(key, sy.astype(np.int64) * BOX_W + sx)
                cur = grid[sy, sx]
                take = dist[idx, k] <= dist[idx, cur] * UNCUT_FAC + UNCUT_PAD
                if take.any():
                    grid[sy[take], sx[take]] = k
                    moved += int(take.sum())
        if not moved:
            break


def repair(grid, dist, key, med, n, lockm, lab):
    """浮块归位：一画写出来必须是连着的 —— 要么自己连成一片，要么贴着先写过的墨。

    两头都不沾的碎块就是分错了，动画里成了凭空冒出来的一小撮（就是「残留」）；
    把它整块让给周围占得最多的那一画。

    还有一条硬约束：一画写出来的墨只可能落在一座墨岛上。「领」里令字的捺在
    makemeahanzi 里伸得长，中线一直探到右边页字头上，于是捺把页字的一小块也
    划拉过来了 —— 隔着一整片空白，动画里就是凭空亮起一小撮。跨岛的那块一律退回去。
    """
    for _ in range(REPAIR_PASSES):
        moved = 0
        for k in range(n):
            mk = (grid == k).astype(np.uint8)
            if not mk.any():
                continue
            ncomp, lb = cv2.connectedComponents(mk, connectivity=8)
            if ncomp <= 2:
                continue
            # 哪一块是主体：看笔尖（中线采样点）在哪块里走得最多。
            # 不能只看谁大 —— 「仪」的点和捺的起笔都被判给了第 3 画，702 对 687，
            # 按大小选主体正好选反，对的那个点反被当成碎片挪走了。
            mp = med[k][0]
            px = np.clip(mp[:, 0].astype(int), 0, BOX_W - 1)
            py = np.clip(mp[:, 1].astype(int), 0, BOX_H - 1)
            hit = np.bincount(lb[py, px], minlength=ncomp)
            hit[0] = 0
            comps = sorted(((int(hit[c]), int((lb == c).sum()), c) for c in range(1, ncomp)),
                           reverse=True)
            # claim_orphans 认领下来的整块是这一画最确凿的墨，它就是主体，
            # 剩下那些（多半是判错抢来的）都该让出去。
            lock = [c for c in range(1, ncomp) if lockm[lb == c].any()]
            if lock:
                home = int(lab[lb == lock[0]][0])             # 这一画该待的那座墨岛
            else:
                isl = np.bincount(lab[mk > 0], minlength=int(lab.max()) + 1)
                isl[0] = 0
                home = int(isl.argmax())
            stay = [t for t in comps if int(lab[lb == t[2]][0]) == home]
            earlier = (grid >= 0) & (grid < k)

            def touches_earlier(c):
                s = lb == c
                return bool(earlier[(cv2.dilate(s.astype(np.uint8), K8) > 0) & ~s].any())

            if lock:
                main = max(lock, key=lambda c: int((lb == c).sum()))
            elif not stay:
                main = comps[0][2]
            else:
                # 主体优先挑「接着先写过的墨」的那一块。光看笔尖走得多会挑错 ——
                # 「愿」里厂字那一撇，makemeahanzi 的中线大半压在白字头上，
                # 于是把白字那一块当成了主体，真正接着上一横往下撇的那块反被让了出去。
                main = max(stay, key=lambda t: (touches_earlier(t[2]), t[0], t[1]))[2]
            for _h, cnt, c in comps:
                if c == main or c in lock:                   # 主体和认领块不动
                    continue
                sel = lb == c
                ring = (cv2.dilate(sel.astype(np.uint8), K8) > 0) & ~sel
                # 同岛上的碎块贴着先写过的墨就留着（看起来是连着的）；
                # 跨岛的、以及已经认领到别处的那一画的碎块，一律退回去。
                if int(lab[sel][0]) == home and not lock and earlier[ring].any():
                    continue
                sy, sx = np.nonzero(sel)
                dd = dist[np.searchsorted(key, sy.astype(np.int64) * BOX_W + sx)].mean(0).copy()
                dd[k] = np.inf
                nb = np.unique(grid[ring])
                nb = nb[(nb >= 0) & (nb != k)]
                # 在挨着的那几画里挑中线最近的一画。按「周围谁的像素多」投票会投错：
                # 碎块往往正贴着另一画的粗腰，而它其实是第三画被压断的一截。
                if len(nb):
                    dst = int(nb[np.argmin(dd[nb])])
                else:
                    # 四周一个邻居都没有的孤块：两画会互相推让（「仪」那一点就是
                    # 甲判给乙、乙判给甲，六轮下来还在弹）。判一次就锁住，别再动。
                    dst = int(dd.argmin())
                    lockm[sel] = True
                grid[sel] = dst
                moved += cnt
        if not moved:
            break


def claim_orphans(inkm, masks, meds):
    """整块认领：没有任何一画的中线经过的墨块，整块判给轮廓最贴它的那一画。

    makemeahanzi 和霞鹜文楷偶尔把同一个部件摆在不一样的位置 ——「仪」里义字的
    那一点，前者点在捺的左上方，后者点在撇和捺当中，差了三十多像素，register
    的 8 px 够不着。这种时候按「离哪条中线近」判必错（撇的中线反而更近），而且
    这一块会在 repair 里被两画来回推、永远收敛不了。改成拿各画的轮廓去套这块墨，
    谁套得最严实就是谁的 —— 一个点大小的墨块，只有「点」那一画套得上。
    判一次就锁死，后面 uncut/repair 都不许再动它。
    """
    ncomp, lab = cv2.connectedComponents(inkm.astype(np.uint8), connectivity=8)
    if ncomp <= 2:
        return lab, {}
    near = lab.astype(np.float32)                            # 中线可能擦出墨边，就近吸附几格
    for _ in range(4):
        g = cv2.dilate(near, K8)
        near = np.where(near == 0, g, near)
    near = near.astype(np.int32)
    taken = np.zeros(ncomp, dtype=bool)
    for mp, _ms in meds:
        px = np.clip(mp[:, 0].astype(int), 0, BOX_W - 1)
        py = np.clip(mp[:, 1].astype(int), 0, BOX_H - 1)
        h = np.bincount(near[py, px], minlength=ncomp)
        h[0] = 0
        if h.sum():
            taken[int(h.argmax())] = True
    out = {}
    for c in range(1, ncomp):
        if taken[c]:
            continue
        blob = lab == c
        area = int(blob.sum())
        by, bx = np.nonzero(blob)
        cy, cx = by.mean(), bx.mean()
        far = cv2.distanceTransform((~blob).astype(np.uint8), cv2.DIST_L2, 3)
        best = (-1.0, 0)
        for k, mk in enumerate(masks):
            my, mx = np.nonzero(mk)
            if len(my) == 0:
                continue
            oy = int(round(cy - my.mean()))                  # 先把轮廓重心搬到墨块重心上
            ox = int(round(cx - mx.mean()))
            inter = 0
            for dy in range(oy - ORPHAN_RANGE, oy + ORPHAN_RANGE + 1, 2):
                for dx in range(ox - ORPHAN_RANGE, ox + ORPHAN_RANGE + 1, 2):
                    yy, xx = my + dy, mx + dx
                    ok = (yy >= 0) & (yy < BOX_H) & (xx >= 0) & (xx < BOX_W)
                    inter = max(inter, int(blob[yy[ok], xx[ok]].sum()))
            mp = meds[k][0]
            px = np.clip(mp[:, 0].astype(int), 0, BOX_W - 1)
            py = np.clip(mp[:, 1].astype(int), 0, BOX_H - 1)
            d = float(far[py, px].min())
            # 三件事都要：这一画的轮廓基本被这块墨吞掉（说明它只有这么大）、
            # 两者形状也对得上、而且它的中线本来就在附近。少一件就会判错 ——
            # 光看贴合度，「领」里令字那一点会被捺抢走（捺也小）；光看中线远近，
            # 「仪」里那一点会被撇抢走（撇的中线正好压过去）。
            sc = (inter / len(my)) * (2 * inter / (len(my) + area)) * np.exp(-d / ORPHAN_DECAY)
            if sc > best[0]:
                best = (sc, k)
        out[c] = best[1]
    return lab, out


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

    ink  = np.stack([ix, iy], 1).astype(np.float32)          # (N,2)
    inkm = np.zeros((BOX_H, BOX_W), dtype=bool)
    inkm[iy, ix] = True

    masks, meds = [], []
    for k in range(n):
        m = Image.new("1", (BOX_W, BOX_H), 0)
        dr = ImageDraw.Draw(m)
        for sub in strokes[k]:
            dr.polygon([tuple(q) for q in to_box(sub)], fill=1)
        masks.append(np.asarray(m).copy())
        meds.append(densify(to_box(medians[k]), MEDIAN_STEP))
    masks, meds = register(inkm, masks, meds)
    lab, forced = claim_orphans(inkm, masks, meds)

    N = len(ink)
    dist   = np.empty((N, n), dtype=np.float32)              # 到各画中线的最近距离
    inside = np.zeros((N, n), dtype=bool)                    # 是否落在各画的轮廓里
    med    = []                                              # 各画的中线采样点和弧长位置
    for k in range(n):
        inside[:, k] = masks[k][iy, ix]
        mp, ms = meds[k]
        mp = mp.astype(np.float32)
        med.append((mp, ms))
        dist[:, k] = np.sqrt(((ink[:, None, :] - mp[None, :, :]) ** 2).sum(-1)).min(1)

    owner = np.where(inside, dist * INSIDE_BONUS, dist).argmin(1).astype(np.int16)

    # 交叉处归先写的那一画。后写的压在先写的上面，不该把先写的那一笔挖断 ——
    # 挖断了它写出来中间是一段黑的，很脏；归先写的之后，后写的那一画路过这儿时
    # 像素已经是白的，看起来就是自然地压过去。
    # 只认「两条中线都从这儿附近过」的真交叉：轮廓偶尔会胖到盖住别的笔画，那种不算，
    # 否则会把像素提前一大截放出来。
    multi = inside.sum(1) >= 2
    if multi.any():
        dm = np.where(inside[multi], dist[multi], np.inf)
        near = dm <= (dm.min(1, keepdims=True) * CROSS_SLACK + CROSS_PAD)
        owner[multi] = near.argmax(1)                        # 第一个 True = 最早的那一画

    grid = np.full((BOX_H, BOX_W), -1, dtype=np.int16)
    grid[iy, ix] = owner
    lockm = np.zeros((BOX_H, BOX_W), dtype=bool)
    for c, k in forced.items():
        sel = lab == c
        grid[sel] = k
        lockm[sel] = True
    key = iy.astype(np.int64) * BOX_W + ix                   # 行优先，已排好序，可二分
    uncut(grid, dist, key, n, lockm)
    repair(grid, dist, key, med, n, lockm, lab)
    owner = grid[iy, ix]

    # 画内先后：按最近中线采样点的弧长位置排。归属动过的像素也在这儿一并重算。
    out, orphan = [], 0
    for k in range(n):
        sel = np.nonzero(owner == k)[0]
        if len(sel) == 0:
            orphan += 1
            out.append(np.zeros((0, 2), dtype=np.uint8))
            continue
        mp, ms = med[k]
        d = np.sqrt(((ink[sel][:, None, :] - mp[None, :, :]) ** 2).sum(-1))
        sel = sel[np.argsort(ms[d.argmin(1)], kind="stable")]
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
