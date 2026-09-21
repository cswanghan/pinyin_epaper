#!/usr/bin/env python3
"""一次性校验（不属于管线）：笔顺动画的像素划分是否就是成品字的墨迹。

对每个字检查两件事：
  1. out/anim/<id>.bin 里各画像素的并集 == out/img565/<id>.bin 大字框内全部非黑像素
     （不缺、不多、不重复）
  2. 框外没有墨 —— 大字墨迹必须整个落在这个框里，否则动画会漏掉笔画的边缘

用法: python3 _verify_anim.py [抽样字数，默认 40；0 = 全量]
只用标准库（本机 content/.venv 里没装 numpy，gen_anim.py 是另找的解释器跑的）。
"""
import json
import random
import struct
import sys
import time
from array import array
from pathlib import Path

BASE = Path(__file__).parent
SCR_W, SCR_H = 368, 448


def load_screen(cid):
    raw = (BASE / f"out/img565/{cid:04d}.bin").read_bytes()
    assert len(raw) == SCR_W * SCR_H * 2, f"图大小 {len(raw)}"
    a = array("H")
    a.frombytes(raw)
    if sys.byteorder == "little":
        a.byteswap()
    return a


def main():
    n_sample = int(sys.argv[1]) if len(sys.argv) > 1 else 40
    recs = json.loads((BASE / "out/chars.json").read_text(encoding="utf-8"))
    if n_sample:
        random.seed(7)
        recs = random.sample(recs, n_sample)

    bad = []
    t0 = time.time()
    for r in recs:
        cid = int(r["id"])
        b = (BASE / f"out/anim/{cid:04d}.bin").read_bytes()
        if b[:4] != b"PSA1":
            bad.append((r["char"], "magic"))
            continue
        n, W, H = b[4], b[5], b[6]
        ox, oy = struct.unpack("<HH", b[8:12])
        cnt = struct.unpack(f"<{n}H", b[12 : 12 + 2 * n])
        off = 12 + 2 * n
        owner = set()
        dup = 0
        for c in cnt:
            seg = b[off : off + 2 * c]
            off += 2 * c
            for j in range(0, 2 * c, 2):
                k = (seg[j], seg[j + 1])
                if k in owner:
                    dup += 1
                owner.add(k)

        a = load_screen(cid)
        ink = set()
        outside = 0
        for y in range(SCR_H):
            for x, v in enumerate(a[y * SCR_W : (y + 1) * SCR_W]):
                if not v:
                    continue
                if ox <= x < ox + W and oy <= y < oy + H:
                    ink.add((x - ox, y - oy))
                elif 90 <= y < 340:          # 大字所在的那一带，框外不该有墨
                    outside += 1
        if dup or owner != ink or outside:
            bad.append((r["char"], f"重复 {dup}，差集 {len(owner ^ ink)}，框外 {outside}"))

    print(f"校验 {len(recs)} 字：不合格 {len(bad)} 个 {bad[:8]}")
    print(f"耗时 {time.time() - t0:.1f} s")


if __name__ == "__main__":
    main()
