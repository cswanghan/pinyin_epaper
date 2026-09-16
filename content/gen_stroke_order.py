#!/usr/bin/env python3
"""导出汉字笔顺（笔画名序列）→ data/stroke_order.txt

产物随代码提交，日常不用跑这个脚本 —— 它要 npm 和一份 30MB 的字形库。
跑 gen_stroke.py 就够了，那个只读已经生成好的 data/stroke_order.txt。

两步:
  1. cnchar-order 给出每个字的笔画名序列。它有 5 组笔画分不开，会输出成
     「横撇|横钩」这样的二选一，另有一批把点写成「点2」。
     二选一的处数: 全表 6811 字里 2544 处，学习机实际用的 2500 字里 834 处。
  2. 用 makemeahanzi 的笔画中线坐标按字形把这 5 组判开。两个几何量:

       尾段占比 = 最大拐点之后的长度 / 整笔长度
         宀的横钩尾巴短(≈0.2)，又的横撇尾巴长(≈0.7)
         阝的横撇弯钩(≈0.17) 对 乃的横折折折钩(≈0.8)
       净竖直位移 / 整笔长度
         斜钩从左上直落右下(≈-0.55)，卧钩是个碗、终点回到起点高度(≈-0.08)

     第二个量的方向和阈值不写死 —— 拿「心忘忠念怒」和「我成戈戏战」当锚字现算，
     免得记错坐标系正负。第一版用的是纵横跨度比，被部件挤压污染了（同样是戋，
     「钱」里舒展判对、「盏」里压扁判错），换成净位移除以自身长度才免疫缩放。

残留误差（实测，不是估计。注意两种口径: 2500 字工作集 / 全表 6811 字）:
  横撇|横钩   工作集 625 处里约 145 处（23%）落在判据中间带 0.35-0.65，两边都不像，
              只能按阈值一刀切 —— 这一组本来就不是干净的双峰
  斜钩|卧钩   全表 335 处里约 4 处判错（尧/民/弋族里被挤得特别扁的那几个）；
              工作集内 119 处
  其余三组干净可分
  也就是 2500 字共 23020 笔里，约 150 处（0.7%）笔画名可能念得不准。
  念错的只是这一画叫什么，笔顺的顺序本身不受影响。

依赖:
  cd content && npm i      # 按 content/package.json 装 cnchar 3.2.6（node_modules 已 gitignore）
  # 必须在 content/ 下装、且 content/ 必须有 package.json: node 是按「js 文件所在目录」
  # 逐级往上找 node_modules 的，没有 package.json 时 npm 也会一路往上装到 ~ 去
  # 锁 3.2.6: 下面那些消歧阈值是按这个版本的输出调的，换版本要重新核对判对判错的字
  curl -fL -o graphics.txt https://raw.githubusercontent.com/skishore/makemeahanzi/master/graphics.txt
  （注意别走 jsdelivr，它对 >20MB 的文件返回一句错误文本但退出码是 0）

用法: python gen_stroke_order.py [--graphics <路径>]    默认找 ./graphics.txt
"""
import json, math, subprocess, sys, tempfile
from collections import Counter
from pathlib import Path

BASE = Path(__file__).parent
OUT  = BASE / "data/stroke_order.txt"

NODE_JS = r"""
const fs = require('fs');
const cnchar = require('cnchar');
const order  = require('cnchar-order');
cnchar.use(order.default || order);          // 不 use 的话 stroke(c,'order') 只返回笔画数
const out = [];
for (const c of fs.readFileSync(process.argv[2], 'utf8')) {
  if (!/[一-龥]/.test(c)) continue;
  try {
    const r = cnchar.stroke(c, 'order', 'name');
    const names = Array.isArray(r[0]) ? r[0] : r;
    if (names && names.length && typeof names[0] === 'string') out.push(c + '\t' + names.join(','));
  } catch (e) { /* 生僻字查不到，跳过 */ }
}
fs.writeFileSync(process.argv[3], out.join('\n'), 'utf8');
"""

# 「点2」其实就是点，cnchar 用它区分同一个字里的第二个点，对朗读没意义
MERGE = {"点2": "点"}

# 判据 t=尾段占比  d=净竖直位移/长度；(低位名, 高位名, 阈值)
# 斜钩|卧钩 的阈值由锚字现算，这里的 None 是占位
RULES = {
    "横撇|横钩":          ("t", "横钩",     "横撇",       0.50),
    "横折折折钩|横撇弯钩": ("t", "横撇弯钩", "横折折折钩", 0.50),
    "横折折|横折弯":       ("t", "横折折",   "横折弯",     0.50),
    "竖折撇|竖折折":       ("t", "竖折撇",   "竖折撇",     0.50),   # 只 11 处，都念竖折撇
    "斜钩|卧钩":          ("d", None,       None,         None),
}
ANCHOR_LOW  = set("心忘忠念怒忍志思")   # 一定是卧钩
ANCHOR_HIGH = set("我成戈戏战或武咸")   # 一定是斜钩


def run_cnchar(chars: str) -> dict:
    # 临时 js 必须落在 BASE 下: node 按「js 文件所在目录」逐级往上找 node_modules，
    # 不看进程 cwd —— 放进 /tmp 会 Cannot find module 'cnchar'
    js = BASE / "_cnchar_tmp.js"
    js.write_text(NODE_JS, encoding="utf-8")
    try:
        with tempfile.TemporaryDirectory() as td:
            src, dst = Path(td) / "in.txt", Path(td) / "out.txt"
            src.write_text(chars, encoding="utf-8")
            r = subprocess.run(["node", str(js), str(src), str(dst)], capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(f"node 失败（在 content/ 下跑过 npm i cnchar cnchar-order 吗？）:\n{r.stderr[:500]}")
            return {ln.split("\t")[0]: ln.split("\t")[1].split(",")
                    for ln in dst.read_text(encoding="utf-8").splitlines() if "\t" in ln}
    finally:
        js.unlink(missing_ok=True)


def feats(p):
    """(尾段占比, 净竖直位移/长度)；点太少算不出返回 None"""
    if len(p) < 3:
        return None
    best, bi = -1, 1
    for i in range(1, len(p) - 1):
        a = (p[i][0] - p[i-1][0], p[i][1] - p[i-1][1])
        b = (p[i+1][0] - p[i][0], p[i+1][1] - p[i][1])
        na, nb = math.hypot(*a), math.hypot(*b)
        if na < 1 or nb < 1:
            continue
        ang = math.acos(max(-1, min(1, (a[0]*b[0] + a[1]*b[1]) / (na*nb))))
        if ang > best:
            best, bi = ang, i
    seg = lambda s, e: sum(math.hypot(p[j+1][0]-p[j][0], p[j+1][1]-p[j][1]) for j in range(s, e))
    total = seg(0, len(p) - 1)
    if total < 1:
        return None
    return seg(bi, len(p) - 1) / total, (p[-1][1] - p[0][1]) / total


def anchor_d(order, medians, chars):
    """锚字里所有「斜钩|卧钩」笔的净竖直位移"""
    out = []
    for c in chars:
        for k, n in enumerate(order.get(c, [])):
            if n != "斜钩|卧钩" or k >= len(medians[c]):
                continue
            f = feats(medians[c][k])
            if f:
                out.append(f[1])
    return out


def main():
    gpath = Path(sys.argv[sys.argv.index("--graphics") + 1]) if "--graphics" in sys.argv \
            else BASE / "graphics.txt"
    if not gpath.exists():
        sys.exit(f"缺 {gpath}，见本脚本顶部的 curl 命令")

    medians = {}
    for line in gpath.open(encoding="utf-8"):
        d = json.loads(line)
        medians[d["character"]] = d["medians"]
    print(f"字形库 {len(medians)} 字 ← {gpath}")

    order = run_cnchar("".join(medians))
    print(f"cnchar 给出笔顺 {len(order)} 字（{len(medians)-len(order)} 个查不到）")

    # 斜钩|卧钩: 用锚字定方向和阈值
    lows  = anchor_d(order, medians, ANCHOR_LOW)
    highs = anchor_d(order, medians, ANCHOR_HIGH)
    if not lows or not highs:
        sys.exit("锚字没匹配上，字形库版本可能变了")
    mw, mx = sum(lows)/len(lows), sum(highs)/len(highs)
    # 自动中点会把尧/民/弋族那一桶误判，往卧钩一侧挪一点；见顶部「残留误差」
    th = (mw + mx) / 2 + 0.07
    lo, hi = ("卧钩", "斜钩") if mw < mx else ("斜钩", "卧钩")
    RULES["斜钩|卧钩"] = ("d", lo, hi, th)
    print(f"斜钩|卧钩 锚点: 卧钩字 {mw:+.3f}  斜钩字 {mx:+.3f} → 阈值 {th:+.3f}，低位「{lo}」")

    stat, rows = Counter(), []
    for c in sorted(order):
        names = list(order[c])
        for k, n in enumerate(names):
            if n in MERGE:
                names[k] = MERGE[n]
                stat[f"{n}→{MERGE[n]}"] += 1
                continue
            if n not in RULES:
                continue
            which, l, h, t = RULES[n]
            f = feats(medians[c][k]) if k < len(medians[c]) else None
            v = None if f is None else f[0 if which == "t" else 1]
            names[k] = l if v is None else (h if v >= t else l)
            stat[f"{n}→{names[k]}" + ("（无字形）" if v is None else "")] += 1
        rows.append((c, names))

    OUT.write_text(
        "# 汉字笔顺（笔画名序列）—— 由 gen_stroke_order.py 生成，见那个脚本的顶部注释\n"
        "# 格式: 汉字 <TAB> 笔画名,笔画名,…\n"
        "# 数据源 cnchar-order 3.2.6；它有 5 组笔画分不开（如「横撇|横钩」），\n"
        "# 已用 makemeahanzi 的笔画中线坐标按字形判开，判据和残留误差见生成脚本。\n"
        + "\n".join(f"{c}\t{','.join(ns)}" for c, ns in rows) + "\n",
        encoding="utf-8")
    print("消歧: " + " | ".join(f"{k} {v}" for k, v in sorted(stat.items())))
    print(f"笔画名 {len({n for _, ns in rows for n in ns})} 种，{len(rows)} 字 → {OUT}")


if __name__ == "__main__":
    main()
