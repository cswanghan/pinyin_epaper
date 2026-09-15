#!/usr/bin/env python3
"""生成「学习清单」—— 把 2500 字切成若干组，每组带语音跳转命令词

分组依据（教学顺序，不是字频）:
  第 1 批: 《义务教育语文课程》(2022) 识字写字教学基本字表（300 字，按笔画排列）
           —— 这是官方指定的"最该先学"的基础字
  第 2 批: 常用字表一剩余部分，按现代汉语字频排序

  （纯按字频排会把"的一是不了在有"排最前，那是虚词，一年级课本绝不会先教）

命令词: 「<词>的<字>」，每字最多 MAX_WORDS 个词，按下面的顺序取、去重:
  1. data/phrase_extra.txt 里人工加的词（如「谷 云谷」），也可以用「-词」剔除
  2. 屏幕上的词（chars.json 的 words）
  3. 常用词: jieba 词典里含这个字、词频最高的两字词（去掉人名地名等专名）
  —— 屏幕词按 HSK 难度挑，人张口说的词不一定在里面（「如」屏幕上是「比如」，
     常说的是「如果」）。每字只登记一个词时，第 2 组常用前 3 个词只覆盖 14%。
  句末的字用它在这个词里的读音（「爪子的爪」是 zhua；单念「爪」pypinyin 给 zhao）。
  常用词只取卡片上教的读音（chars.json 的 readings），「什物的什」「呢绒的呢」这类不要。
  同一句说法落在组里两个字上（「十五的十」和「事物的事」都是 shi wu de shi，模型不分声调）:
  屏幕词/人工词优先（卡片上印着，必须能说）；两边都是常用词就都不登记 —— 只给一个的话，
  想说另一个字的人会跳错。
  不注册「只说词」: 一个词里有两个字，「如果」本身说不清是指哪个，名额留给更多的词。

  写法取自 esp-sr Kconfig 默认值: 无声调拼音空格分隔，多种说法用逗号分隔:
  "ru guo de ru,bi ru de ru"

用法:
  gen_scopes.py                  重新分组 → out/scope/（需要 data/ 下的字表、字频表）
  gen_scopes.py --rephrase DIR   只重写 DIR 里现有清单的命令词 → out/scope/
                                 分组、文件名、字序、注释都不变（进度按文件名记，不会丢）。
                                 手头只有 SD 卡上的 pinyin/scope 和 chars.json 时用这个。

输出 out/scope/NN_*.txt，制表符分隔: <字id>\t<汉字>\t<命令词>
家长可照此格式手写课本单元的清单丢进 SD 卡。
"""
import argparse, csv, json, sys, unicodedata
from pathlib import Path

import jieba
from pypinyin import pinyin, Style

BASE  = Path(__file__).parent
CHARS = BASE / "out/chars.json"
FREQ  = BASE / "data/hanzi-data/现代汉语汉字频率表.csv"
BASIC = BASE / "data/hanzi-chars/data-charlist/《义务教育语文课程》（2022年版）识字写字教学基本字表.txt"
EXTRA = BASE / "data/phrase_extra.txt"
OUT   = BASE / "out/scope"

GROUP_SIZE = 50
MAX_WORDS  = 6      # 每字最多几个词: 50 × 6 + 27 固定 + 50 选组 = 377 条
MN_LIMIT   = 400    # MultiNet7 一次最多注册的说法条数
FIXED_CMDS = 27     # 固件的固定命令条数（app_sr.cpp FIXED 表）
LINE_MAX   = 255    # 一个字的命令词串最长多少字节（app_sr.cpp add_phrases 的缓冲区）
SKIP_POS   = {"nr", "nrt", "nrfg", "ns", "nt", "nz"}    # 人名、地名、机构名等专名


def py_flat(text: str) -> str:
    return " ".join(p[0] for p in pinyin(text, style=Style.NORMAL)).replace("ü", "v")


def toneless(py: str) -> str:
    """带调拼音 → 无声调，ü 写成 v（与 py_flat 一致）"""
    s = unicodedata.normalize("NFD", py)
    s = "".join(c for c in s if not unicodedata.combining(c) or c == "̈")
    return unicodedata.normalize("NFC", s).replace("ü", "v")


def has_dup_syllable(py: str) -> bool:
    """检测相邻重复音节，如「别的的的」→ bie de de de。
    这类短语拗口且识别率低，应换个词。"""
    syl = py.split()
    return any(syl[i] == syl[i + 1] for i in range(len(syl) - 1))


def read_charfile(path: Path) -> list:
    seen, out = set(), []
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            continue
        for c in line:
            if "一" <= c <= "鿿" and c not in seen:
                seen.add(c); out.append(c)
    return out


def load_freq_rank() -> dict:
    rank = {}
    if not FREQ.exists():
        print(f"⚠ 字频表缺失: {FREQ}", file=sys.stderr); return rank
    with open(FREQ, encoding="utf-8") as f:
        for row in csv.DictReader(f):
            ch = (row.get("汉字") or "").strip()
            if ch and ch not in rank:
                rank[ch] = int(row["id"])
    return rank


def load_common_words() -> dict:
    """jieba 词典 → {字: [含这个字的两字词，词频从高到低]}"""
    by_char = {}
    with open(Path(jieba.__file__).parent / "dict.txt", encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            w, freq, pos = parts[0], int(parts[1]), parts[2]
            if len(w) != 2 or w[0] == w[1] or pos in SKIP_POS:
                continue
            if not all("一" <= c <= "鿿" for c in w):
                continue
            for c in set(w):
                by_char.setdefault(c, []).append((freq, w))
    return {c: [w for _, w in sorted(v, reverse=True)] for c, v in by_char.items()}


def load_extra() -> tuple:
    """data/phrase_extra.txt → ({字: [加的词]}, {字: {剔除的词}})"""
    add, drop = {}, {}
    if EXTRA.exists():
        for line in EXTRA.read_text(encoding="utf-8").splitlines():
            parts = line.split()
            if not parts or parts[0].startswith("#"):
                continue
            for w in parts[1:]:
                if w.startswith("-"):
                    drop.setdefault(parts[0], set()).add(w[1:])
                else:
                    add.setdefault(parts[0], []).append(w)
    return add, drop


def phrase_candidates(rec, common, extra) -> list:
    """一个字的全部候选说法 [(说法, 词, 是否屏幕词/人工词)]，按优先级: 人工加的 → 屏幕上的 → 常用词"""
    ch = rec["char"]
    add, drop = extra
    curated = add.get(ch, []) + list(rec.get("words", []))
    readings = {toneless(r) for r in rec.get("readings") or [rec["pinyin"]]}
    out = {}
    for w in dict.fromkeys(curated + common.get(ch, [])[:20]):
        if ch not in w or w in drop.get(ch, ()):
            continue
        syl = py_flat(w).split()
        if len(syl) != len(w):
            continue                                   # 含非汉字
        last = syl[w.index(ch)]                        # 句末用这个字在词里的读音
        if w not in curated and last not in readings:
            continue                                   # 常用词只要卡片上教的读音
        p = " ".join(syl + ["de", last])
        if not has_dup_syllable(p):                    # 如「别的的的」，换下一个词
            out.setdefault(p, (w, w in curated))       # 同音的词只留一个，屏幕词排在前面
    return [(p, w, cur) for p, (w, cur) in out.items()]


def group_phrases(recs, common, extra) -> tuple:
    """一组字的命令词，返回 (每字的说法列表, 组内同音撞车的说法数)"""
    cands = [phrase_candidates(r, common, extra) for r in recs]
    owners = {}                                        # 说法 → [(第几个字, 是否屏幕词/人工词)]
    for i, c in enumerate(cands):
        for p, _, cur in c:
            owners.setdefault(p, []).append((i, cur))
    winner = {}                                        # 说法 → 登记给第几个字，None = 谁都不给
    for p, own in owners.items():
        cur = [i for i, c in own if c]
        winner[p] = cur[0] if cur else (own[0][0] if len(own) == 1 else None)
    out = []
    for i, (r, c) in enumerate(zip(recs, cands)):
        ph = []
        for p, _, _ in c:
            if winner[p] != i or len(",".join(ph + [p])) > LINE_MAX:
                continue
            ph.append(p)
            if len(ph) == MAX_WORDS:
                break
        out.append(ph or [py_flat(r["char"])])         # 没有合适的词（多为虚词）→ 退化为念单字
    return out, sum(1 for own in owners.values() if len(own) > 1)


def is_degraded(ph) -> bool:
    return len(ph) == 1 and " " not in ph[0]


def regroup(recs, common, extra) -> list:
    """重新分组，返回 [(文件名, 行, 命令条数, 退化字数, 冲突数)]"""
    by_char = {r["char"]: r for r in recs}
    rank = load_freq_rank()

    basic = [c for c in read_charfile(BASIC) if c in by_char]   # 过滤掉偏旁部首
    basic_set = set(basic)
    rest = sorted((r for r in recs if r["char"] not in basic_set),
                  key=lambda r: rank.get(r["char"], 10**9))
    ordered = [by_char[c] for c in basic] + rest
    print(f"排序: 基础字表 {len(basic)} 字（按笔画）+ 其余 {len(rest)} 字（按字频）")

    files = []
    n_groups = (len(ordered) + GROUP_SIZE - 1) // GROUP_SIZE
    for gi in range(n_groups):
        chunk = ordered[gi * GROUP_SIZE:(gi + 1) * GROUP_SIZE]
        is_basic = (gi + 1) * GROUP_SIZE <= len(basic)
        tag = "基础字" if is_basic else "常用字"
        lines = [
            f"# 学习清单 第 {gi+1} 组（{tag}，第 {gi*GROUP_SIZE+1}-{gi*GROUP_SIZE+len(chunk)} 字）",
            "# 格式: 字id <TAB> 汉字 <TAB> 语音命令词（无声调拼音，多说法用逗号分隔）",
            "# 家长可照此格式自建清单：把课本单元的生字表填进来即可",
        ]
        phs, clash = group_phrases(chunk, common, extra)
        lines += [f"{r['id']}\t{r['char']}\t{','.join(ph)}" for r, ph in zip(chunk, phs)]
        files.append((f"{gi+1:02d}_{tag}{gi*GROUP_SIZE+1}-{gi*GROUP_SIZE+len(chunk)}.txt", lines,
                      sum(map(len, phs)), sum(map(is_degraded, phs)), clash))
    return files


def rephrase(src: Path, recs, common, extra) -> list:
    """只重写现有清单的命令词，其余原样保留，返回 [(文件名, 行, 命令条数, 退化字数, 冲突数)]"""
    by_id = {r["id"]: r for r in recs}
    srcs = sorted(f for f in src.glob("*.txt") if not f.name.startswith("._"))
    if not srcs:
        sys.exit(f"{src} 里没有清单")
    files = []
    for f in srcs:
        lines = f.read_text(encoding="utf-8").splitlines()
        rows = []                                      # (行号, 字记录)
        for i, line in enumerate(lines):
            if not line.strip() or line.startswith("#"):
                continue
            parts = line.split("\t")
            rec = by_id.get(int(parts[0])) if parts[0].strip().isdigit() else None
            if not rec or len(parts) < 2 or rec["char"] != parts[1].strip():
                print(f"⚠ {f.name} 第 {i+1} 行对不上字表，原样保留: {line}", file=sys.stderr)
                continue
            rows.append((i, rec))
        phs, clash = group_phrases([r for _, r in rows], common, extra)
        for (i, rec), ph in zip(rows, phs):
            lines[i] = f"{rec['id']}\t{rec['char']}\t{','.join(ph)}"
        files.append((f.name, lines, sum(map(len, phs)), sum(map(is_degraded, phs)), clash))
    return files


def main():
    ap = argparse.ArgumentParser(description="生成学习清单（分组 + 语音跳字命令词）")
    ap.add_argument("--rephrase", metavar="DIR", type=Path,
                    help="只重写 DIR 里现有清单的命令词，分组不变（如 SD 卡上的 pinyin/scope）")
    ap.add_argument("--chars", type=Path,
                    help="字表 chars.json；默认 out/chars.json，没有时 --rephrase 用 DIR 上一级的")
    args = ap.parse_args()

    chars = args.chars or CHARS
    if not chars.exists() and args.rephrase:
        chars = args.rephrase.parent / "chars.json"             # SD 卡上 pinyin/chars.json
    recs = json.load(open(chars, encoding="utf-8"))
    common, extra = load_common_words(), load_extra()

    # 先全部算完再清空输出目录 —— DIR 就是 out/scope 时也不会读到一半被删
    files = rephrase(args.rephrase, recs, common, extra) if args.rephrase else regroup(recs, common, extra)

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.txt"):
        old.unlink()
    for name, lines, *_ in files:
        (OUT / name).write_text("\n".join(lines) + "\n", encoding="utf-8")

    counts = [f[2] for f in files]
    limit = MN_LIMIT - FIXED_CMDS - len(files)
    print(f"生成 {len(files)} 组 → {OUT}")
    print(f"跳字命令每组平均 {sum(counts)/len(files):.0f} 条、最多 {max(counts)} 条"
          f"（另有固定 {FIXED_CMDS} + 选组 {len(files)} 条，总上限 {MN_LIMIT}）")
    for name, _, n, *_ in files:
        if n > limit:
            print(f"⚠ {name}: 跳字命令 {n} 条，超出 {n - limit} 条，固件会丢掉排在后面的", file=sys.stderr)
    print(f"组内同音撞车的说法: {sum(f[4] for f in files)} 条（屏幕词优先，都是常用词就两边都不登记）")
    print(f"退化为单字念法的: {sum(f[3] for f in files)} 字（多为虚词，没有合适词组）")
    name, lines = files[0][0], files[0][1]
    print(f"\n=== {name} ===")
    print("\n".join(lines[:8]))


if __name__ == "__main__":
    main()
