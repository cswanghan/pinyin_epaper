#!/usr/bin/env python3
"""生成「学习清单」—— 把 2500 字切成若干组（学习顺序），每个字带查字说法

分组依据（教学顺序，不是字频）:
  第 1 批: 《义务教育语文课程》(2022) 识字写字教学基本字表（300 字，按笔画排列）
           —— 这是官方指定的"最该先学"的基础字
  第 2 批: 常用字表一剩余部分，按现代汉语字频排序

  （纯按字频排会把"的一是不了在有"排最前，那是虚词，一年级课本绝不会先教）

  分组只管学习顺序（「下一个」一组接一组往下走）。查字不分组: 说「如果的如」，
  不管「如」在哪一组都能跳过去。

查字说法: 「<词>的<字>」，每字最多 MAX_WORDS 个词，按下面的顺序取、去重:
  1. data/phrase_extra.txt 里人工加的词（如「谷 云谷」），也可以用「-词」剔除；
     加的词可以不含这个字，当描述用（「她 女字旁」→「女字旁的她」）
  2. 屏幕上的词（chars.json 的 words）
  3. 常用词: jieba 词典里含这个字、词频最高的两字词（去掉人名地名等专名）
  —— 屏幕词按 HSK 难度挑，人张口说的词不一定在里面（「如」屏幕上是「比如」，
     常说的是「如果」）。每字只登记一个词时，第 2 组常用前 3 个词只覆盖 14%。
  句末的字用它在这个词里的读音（「爪子的爪」是 zhua；单念「爪」pypinyin 给 zhao）。
  常用词只取卡片上教的读音（chars.json 的 readings），「什物的什」「呢绒的呢」这类不要。
  「哥哥的哥」「别的的的」这类有相邻重复音节的说法拗口，排在最后，别的词不够时才用。
  同一句说法落在两个字上（「十五的十」和「事物的事」都是 shi wu de shi，模型不分声调）:
  在全部字里比（查字不分组）。屏幕词/人工词优先（卡片上印着，必须能说）；两边都是常用词时，
  一个比另一个常用 CLASH_RATIO 倍以上（如果 38374 : 辱国 3）就给常用的，差不多常用（由于 : 犹豫）
  就都不登记 —— 只给一个的话，想说另一个字的人会跳错。
  不注册「只说词」: 一个词里有两个字，「如果」本身说不清是指哪个。

  固件「对原始文本」（app_sr.cpp）: MultiNet7 一次最多注册 400 条，1.4 万条说法放不下，
  所以命令词表只放固定命令。查字取 MultiNet 逐字听出的拼音里最后的「de X」，跟以 X
  （和容易听混的音节）结尾的说法逐条比。所以说法必须以「de <音节>」结尾。

  写法取自 esp-sr Kconfig 默认值: 无声调拼音空格分隔，多种说法用逗号分隔:
  "ru guo de ru,bi ru de ru"

用法:
  gen_scopes.py                  重新分组 → out/scope/（需要 data/ 下的字表、字频表）
  gen_scopes.py --rephrase DIR   只重写 DIR 里现有清单的说法 → out/scope/
                                 分组、文件名、字序、注释都不变（进度按文件名记，不会丢）。
                                 手头只有 SD 卡上的 pinyin/scope 和 chars.json 时用这个。

输出 out/scope/NN_*.txt，制表符分隔: <字id>\t<汉字>\t<说法>
家长可照此格式手写课本单元的清单丢进 SD 卡（固件把所有清单合起来当查字表）。
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
MAX_WORDS  = 6      # 每字最多几个词
CLASH_RATIO = 100   # 同音的两个常用词，一个比另一个常用这么多倍，就只给常用的（如果 38374 : 辱国 3）
LINE_MAX   = 255    # 一个字的说法串最长多少字节（固件控制台 find 的行缓冲）
SKIP_POS   = {"nr", "nrt", "nrfg", "ns", "nt", "nz"}    # 人名、地名、机构名等专名


def py_flat(text: str) -> str:
    return " ".join(p[0] for p in pinyin(text, style=Style.NORMAL)).replace("ü", "v")


def toneless(py: str) -> str:
    """带调拼音 → 无声调，ü 写成 v（与 py_flat 一致）"""
    s = unicodedata.normalize("NFD", py)
    s = "".join(c for c in s if not unicodedata.combining(c) or c == "̈")
    return unicodedata.normalize("NFC", s).replace("ü", "v")


def has_dup_syllable(py: str) -> bool:
    """检测相邻重复音节，如「别的的的」→ bie de de de"""
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


WORD_FREQ = {}      # 两字常用词 → jieba 词频（load_common_words 填，同音撞车时比谁常用）


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
            WORD_FREQ[w] = freq
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
        if w in drop.get(ch, ()):
            continue
        syl = py_flat(w).split()
        if len(syl) != len(w):
            continue                                   # 含非汉字
        if ch in w:
            last = syl[w.index(ch)]                    # 句末用这个字在词里的读音
        elif w in add.get(ch, ()):
            last = toneless(rec["pinyin"])             # 人工加的描述，不含这个字，如「女字旁的她」
        else:
            continue
        if w not in curated and last not in readings:
            continue                                   # 常用词只要卡片上教的读音
        p = " ".join(syl + ["de", last])
        out.setdefault(p, (w, w in curated))           # 同音的词只留一个，屏幕词排在前面
    # 「哥哥的哥」「别的的的」拗口，排到最后，别的词不够时才用（的/得/德 只有这种说法）
    return sorted(((p, w, cur) for p, (w, cur) in out.items()), key=lambda x: has_dup_syllable(x[0]))


def common_winner(own) -> int | None:
    """一句说法只有常用词想要: 只有一个字，或者它的词比别的都常用得多（如果 : 辱国），就给它；
    差不多常用（由于 : 犹豫）谁都不给 —— 只给一个的话，想说另一个字的人会跳错"""
    if len(own) == 1:
        return own[0][0]
    f = sorted(((WORD_FREQ.get(w, 0), i) for i, _, w in own), reverse=True)
    return f[0][1] if f[0][0] >= CLASH_RATIO * max(f[1][0], 1) else None


def assign_phrases(recs, common, extra) -> tuple:
    """给一批字分配说法（查字不分组，传全部字进来），返回 (每字的说法列表, 撞车的说法数)"""
    cands = [phrase_candidates(r, common, extra) for r in recs]
    owners = {}                                        # 说法 → [(第几个字, 是否屏幕词/人工词, 词)]
    for i, c in enumerate(cands):
        for p, w, cur in c:
            owners.setdefault(p, []).append((i, cur, w))
    winner = {}                                        # 说法 → 登记给第几个字，None = 谁都不给
    for p, own in owners.items():
        cur = [i for i, c, _ in own if c]
        winner[p] = cur[0] if cur else common_winner(own)
    out = []
    for i, (r, c) in enumerate(zip(recs, cands)):
        ph = []
        for p, _, _ in c:
            if winner[p] != i or len(",".join(ph + [p])) > LINE_MAX:
                continue
            ph.append(p)
            if len(ph) == MAX_WORDS:
                break
        out.append(ph or [py_flat(r["char"])])         # 没有合适的词 → 退化为念单字（查不到）
    return out, sum(1 for own in owners.values() if len(own) > 1)


def is_degraded(ph) -> bool:
    return len(ph) == 1 and " " not in ph[0]


def regroup(recs, common, extra) -> tuple:
    """重新分组，返回 ([(文件名, 行)], {字id: 说法}, 撞车数)"""
    by_char = {r["char"]: r for r in recs}
    rank = load_freq_rank()

    basic = [c for c in read_charfile(BASIC) if c in by_char]   # 过滤掉偏旁部首
    basic_set = set(basic)
    rest = sorted((r for r in recs if r["char"] not in basic_set),
                  key=lambda r: rank.get(r["char"], 10**9))
    ordered = [by_char[c] for c in basic] + rest
    print(f"排序: 基础字表 {len(basic)} 字（按笔画）+ 其余 {len(rest)} 字（按字频）")

    phs, clash = assign_phrases(ordered, common, extra)
    files = []
    n_groups = (len(ordered) + GROUP_SIZE - 1) // GROUP_SIZE
    for gi in range(n_groups):
        lo, hi = gi * GROUP_SIZE, min((gi + 1) * GROUP_SIZE, len(ordered))
        is_basic = hi <= len(basic)
        tag = "基础字" if is_basic else "常用字"
        lines = [
            f"# 学习清单 第 {gi+1} 组（{tag}，第 {lo+1}-{hi} 字）",
            "# 格式: 字id <TAB> 汉字 <TAB> 查字说法（无声调拼音「…de 字」，多说法用逗号分隔）",
            "# 家长可照此格式自建清单：把课本单元的生字表填进来即可",
        ]
        lines += [f"{r['id']}\t{r['char']}\t{','.join(ph)}" for r, ph in zip(ordered[lo:hi], phs[lo:hi])]
        files.append((f"{gi+1:02d}_{tag}{lo+1}-{hi}.txt", lines))
    return files, {r["id"]: ph for r, ph in zip(ordered, phs)}, clash


def rephrase(src: Path, recs, common, extra) -> tuple:
    """只重写现有清单的说法，其余原样保留，返回 ([(文件名, 行)], {字id: 说法}, 撞车数)"""
    by_id = {r["id"]: r for r in recs}
    srcs = sorted(f for f in src.glob("*.txt") if not f.name.startswith("._"))
    if not srcs:
        sys.exit(f"{src} 里没有清单")
    parsed, uniq = [], {}                              # uniq: 字id → 字记录，按第一次出现的顺序
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
            uniq.setdefault(rec["id"], rec)
        parsed.append((f.name, lines, rows))

    phs, clash = assign_phrases(list(uniq.values()), common, extra)
    by_ph = dict(zip(uniq, phs))
    files = []
    for name, lines, rows in parsed:
        for i, rec in rows:
            lines[i] = f"{rec['id']}\t{rec['char']}\t{','.join(by_ph[rec['id']])}"
        files.append((name, lines))
    return files, by_ph, clash


def main():
    ap = argparse.ArgumentParser(description="生成学习清单（分组 + 查字说法）")
    ap.add_argument("--rephrase", metavar="DIR", type=Path,
                    help="只重写 DIR 里现有清单的说法，分组不变（如 SD 卡上的 pinyin/scope）")
    ap.add_argument("--chars", type=Path,
                    help="字表 chars.json；默认 out/chars.json，没有时 --rephrase 用 DIR 上一级的")
    args = ap.parse_args()

    chars = args.chars or CHARS
    if not chars.exists() and args.rephrase:
        chars = args.rephrase.parent / "chars.json"             # SD 卡上 pinyin/chars.json
    recs = json.load(open(chars, encoding="utf-8"))
    common, extra = load_common_words(), load_extra()

    # 先全部算完再清空输出目录 —— DIR 就是 out/scope 时也不会读到一半被删
    files, phs, clash = (rephrase(args.rephrase, recs, common, extra) if args.rephrase
                         else regroup(recs, common, extra))

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.txt"):
        old.unlink()
    for name, lines in files:
        (OUT / name).write_text("\n".join(lines) + "\n", encoding="utf-8")

    part = {}                                          # 最后一个音节 → 说法条数（固件按这个分份）
    for ph in phs.values():
        for p in ph:
            s = p.split()
            if len(s) >= 3 and s[-2] == "de":
                part[s[-1]] = part.get(s[-1], 0) + 1
    big = sorted(part.items(), key=lambda x: -x[1])
    print(f"生成 {len(files)} 组 {len(phs)} 字 → {OUT}")
    print(f"查字说法 {sum(part.values())} 条，按最后一个音节分 {len(part)} 份")
    print("最大的几份: " + "  ".join(f"{s} {n}" for s, n in big[:6]))
    print(f"同音撞车的说法: {clash} 条（屏幕词优先；都是常用词时给常用得多的那个，差不多就两边都不登记）")
    by_id = {r["id"]: r for r in recs}
    deg = [by_id[i]["char"] for i, ph in phs.items() if is_degraded(ph)]
    print(f"没有「…的X」说法、查不到的字: {len(deg)} 个 {''.join(deg)}（可在 data/phrase_extra.txt 给它们加词）")
    name, lines = files[0]
    print(f"\n=== {name} ===")
    print("\n".join(lines[:8]))


if __name__ == "__main__":
    main()
