#!/usr/bin/env python3
"""生成笔顺朗读的数据表和音频片段 → out/stroke/

为什么要念笔顺:
  四色墨水屏刷一次 17.6 秒（波形固化在面板 OTP 里，驱动只能发 0x12，改不了，
  也没有单色/快刷模式）。这段时间原本只有「字音 + 词组」约 4 秒有声音，剩下
  十几秒干等。把笔顺念出来正好填上：孩子跟着读音和笔顺在纸上写，写完抬头，
  字刚好出现在屏幕上。

为什么拼片段、不是每字存一整段:
  每字一整段朗读要多占 600 MB，而且笔画之间的停顿被烤进音频里，想调快慢只能
  重跑全部 TTS。拆成 50 多个片段（约 2 MB）之后，停顿变成 config.txt 里的
  stroke_gap_ms，固件照笔顺表把片段排进播放队列即可。

输出:
  out/stroke/bishun.txt   id <TAB> 汉字 <TAB> 笔画编码（一画一字符，表头有对照表）
  out/stroke/s00.wav ..   笔画名，编号 = STROKE_NAMES 下标 = 编码字符
  out/stroke/n01.wav ..   「一画」..「二十三画」
  out/stroke/tail.wav     「看看屏幕上的字」
  out/stroke/again.wav    「再写一遍」（≤4 画的字笔顺念两遍，中间插这句）

笔顺数据来自随代码提交的 data/stroke_order.txt，本脚本不联网取笔顺。
那份数据怎么来的（含 5 组笔画名的字形消歧）见 gen_stroke_order.py，很少需要重跑。

用法: .venv/bin/python gen_stroke.py [--force]
"""
import asyncio, json, sys
from pathlib import Path

from gen_audio import tts, to_wav, cn_num, TRIM

BASE = Path(__file__).parent
DATA = BASE / "data/stroke_order.txt"
OUT  = BASE / "out/stroke"
TMP  = BASE / "out/.tts_tmp"

# 笔画名表 —— 顺序固定，只准往末尾追加。
# 下标同时是 bishun.txt 里的编码字符和 s??.wav 的文件名，中间插一个名字会让
# 已生成的音频、已经写好的 SD 卡全体错位。数据里出现表外的名字时脚本直接报错。
STROKE_NAMES = [
    "横", "竖", "撇", "点", "捺",                                         #  0-4  基本笔画
    "提", "竖钩", "弯钩", "竖弯", "竖弯钩", "竖提",                        #  5-10 竖起的单折单钩
    "撇折", "撇点", "斜钩", "卧钩",                                        # 11-14 斜的
    "横折", "横钩", "横撇", "横折钩", "横折提", "横折弯", "横斜钩",        # 15-21 横起一折
    "横折折", "横折折撇", "横折折折", "横撇弯钩", "横折折折钩",            # 22-26 横起多折
    "竖折撇", "竖折折钩",                                                  # 27-28 竖起多折
]
ENC = "0123456789abcdefghijklmnopqrstuvwxyz"

# 刷屏 17.6 秒里，字音 + 词组实测约 4.0 秒（见 wake_fix.log），剩下的交给笔顺
REFRESH_S = 17.6
HEAD_S    = 4.0
SHORT_MAX = 4          # ≤ 这个画数的字，笔顺念两遍（与固件 app_stroke.cpp 的 SHORT_MAX 一致）
GAP_MIN   = 0.25       # 笔画间停顿的上下限，同固件的 GAP_MIN_MS / GAP_MAX_MS
GAP_MAX   = 2.0


def load_chars() -> dict:
    """id → 汉字。chars.json 是生成物、可能已被清掉，没有就从学习清单还原"""
    cj = BASE / "out/chars.json"
    if cj.exists():
        return {r["id"]: r["char"] for r in json.load(open(cj, encoding="utf-8"))}
    ids = {}
    for f in sorted((BASE / "out/scope").glob("*.txt")):
        for ln in f.read_text(encoding="utf-8").splitlines():
            ln = ln.strip()
            if not ln or ln.startswith("#"):
                continue
            p = ln.split("\t")
            if len(p) >= 2 and p[0].isdigit():
                ids.setdefault(int(p[0]), p[1])
    return ids


def load_order() -> dict:
    d = {}
    for ln in DATA.read_text(encoding="utf-8").splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        c, names = ln.split("\t")
        d[c] = names.split(",")
    return d


def build_table():
    idx = {n: i for i, n in enumerate(STROKE_NAMES)}
    chars, order = load_chars(), load_order()
    if not chars:
        sys.exit("没有 out/scope/*.txt（也没有 out/chars.json），先跑 gen_scopes.py")

    rows, miss, unknown = [], [], set()
    for i in sorted(chars):
        names = order.get(chars[i])
        if not names:
            miss.append(chars[i])
            continue
        bad = [n for n in names if n not in idx]
        if bad:
            unknown.update(bad)
            continue
        rows.append((i, chars[i], "".join(ENC[idx[n]] for n in names)))
    if unknown:
        sys.exit(f"stroke_order.txt 里有 STROKE_NAMES 未收录的笔画名: {sorted(unknown)}\n"
                 f"把它们追加到 STROKE_NAMES 末尾（不要插在中间）再重跑")

    OUT.mkdir(parents=True, exist_ok=True)
    head = ["# 笔顺表 —— 字id <TAB> 汉字 <TAB> 笔画编码（一个字符一画）",
            "# 由 gen_stroke.py 生成，别手改；笔顺数据来源见 data/stroke_order.txt 表头",
            "# 编码字符 → 笔画名（音频是同编号的 s??.wav）:"]
    for k in range(0, len(STROKE_NAMES), 5):
        head.append("#   " + "  ".join(f"{ENC[k+j]}={n}" for j, n in enumerate(STROKE_NAMES[k:k+5])))
    (OUT / "bishun.txt").write_text(
        "\n".join(head) + "\n" + "\n".join(f"{i}\t{c}\t{e}" for i, c, e in rows) + "\n",
        encoding="utf-8")
    print(f"笔顺表 {len(rows)} 字 → {OUT/'bishun.txt'}"
          + (f"   ⚠ 缺笔顺 {len(miss)} 字: {miss[:8]}" if miss else ""))
    return rows


async def gen(key, text, sem, force, stats):
    wav = OUT / f"{key}.wav"
    if wav.exists() and not force:
        stats["skip"] += 1
        return
    async with sem:
        mp3 = TMP / f"stroke_{key}.mp3"
        ok = await tts(text, mp3) and to_wav(mp3, wav, af=TRIM)
        mp3.unlink(missing_ok=True)
    stats["ok" if ok else "fail"] += 1


def report(rows):
    """按实测片段长度算每个字念完要多久，对比 17.6 秒的刷屏窗口"""
    def dur(key):
        f = OUT / f"{key}.wav"
        return (f.stat().st_size - 44) / 32000 if f.exists() else 0.0   # 16k/16bit/单声道

    s = [dur(f"s{i:02d}") for i in range(len(STROKE_NAMES))]
    n = {k: dur(f"n{k:02d}") for k in range(1, 24)}
    tail, again = dur("tail"), dur("again")
    print(f"\n片段时长: 笔画名 {min(x for x in s if x):.2f}-{max(s):.2f} 秒   "
          f"「N 画」{min(n.values()):.2f}-{max(n.values()):.2f} 秒   "
          f"尾句 {tail:.2f} 秒   再写一遍 {again:.2f} 秒")

    gaps, tot = [], []
    floored = ceiled = 0
    for _, _, enc in rows:
        k = len(enc)
        names = sum(s[ENC.index(ch)] for ch in enc)
        body, slots = n.get(k, 0) + names + tail, k
        if k <= SHORT_MAX:                          # 短字念两遍，中间插「再写一遍」
            body += again + names
            slots += k
        gap = (REFRESH_S - HEAD_S - body) / slots if slots else GAP_MIN
        if   gap < GAP_MIN: gap, floored = GAP_MIN, floored + 1
        elif gap > GAP_MAX: gap, ceiled  = GAP_MAX, ceiled + 1
        gaps.append(gap)
        tot.append(HEAD_S + body + gap * slots)

    gaps.sort(); tot.sort()
    near = sum(1 for t in tot if abs(t - REFRESH_S) <= 2.0)
    print(f"  自动停顿: 中位 {gaps[len(gaps)//2]:.2f} 秒   "
          f"念完落在 {REFRESH_S:.1f}±2 秒的 {near} 字（{near*100//len(tot)}%）")
    print(f"  夹到下限 {GAP_MIN:.2f} 秒（笔画多，念不完字就出来）{floored} 字"
          f"（{floored*100//len(tot)}%）   "
          f"夹到上限 {GAP_MAX:.1f} 秒（笔画少，念完还要等）{ceiled} 字")
    print(f"  念完用时: 最短 {tot[0]:.1f}  中位 {tot[len(tot)//2]:.1f}  最长 {tot[-1]:.1f} 秒"
          f"   （HEAD_S={HEAD_S} 秒是估计，固件按 SD 上真实的字音+词组长度算）")


async def main() -> bool:
    rows = build_table()
    TMP.mkdir(parents=True, exist_ok=True)
    maxs = max(len(e) for _, _, e in rows)

    items = {f"s{i:02d}": nm for i, nm in enumerate(STROKE_NAMES)}
    items.update({f"n{k:02d}": f"{cn_num(k)}画" for k in range(1, maxs + 1)})
    items["tail"]  = "看看屏幕上的字"
    items["again"] = "再写一遍"

    sem = asyncio.Semaphore(8)
    stats = {"ok": 0, "skip": 0, "fail": 0}
    await asyncio.gather(*(gen(k, t, sem, "--force" in sys.argv, stats) for k, t in items.items()))
    print(f"音频片段 {len(items)} 个: 新生成 {stats['ok']}  已存在 {stats['skip']}  失败 {stats['fail']}")
    report(rows)
    return stats["fail"] == 0


if __name__ == "__main__":
    sys.exit(0 if asyncio.run(main()) else 1)
