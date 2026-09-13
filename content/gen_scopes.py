#!/usr/bin/env python3
"""生成「学习清单」—— 把 2500 字切成若干组，每组带语音跳转命令词

分组依据（教学顺序，不是字频）:
  第 1 批: 《义务教育语文课程》(2022) 识字写字教学基本字表（300 字，按笔画排列）
           —— 这是官方指定的"最该先学"的基础字
  第 2 批: 常用字表一剩余部分，按现代汉语字频排序

  （纯按字频排会把"的一是不了在有"排最前，那是虚词，一年级课本绝不会先教）

命令词格式（取自 esp-sr Kconfig 默认值写法）:
  无声调拼音空格分隔，同一目标多种说法用逗号分隔："ru guo de ru,ru guo"

输出 out/scope/NN_*.txt，制表符分隔: <字id>\t<汉字>\t<命令词>
家长可照此格式手写课本单元的清单丢进 SD 卡。
"""
import csv, json, sys
from pathlib import Path
from pypinyin import pinyin, Style

BASE  = Path(__file__).parent
CHARS = BASE / "out/chars.json"
FREQ  = BASE / "data/hanzi-data/现代汉语汉字频率表.csv"
BASIC = BASE / "data/hanzi-chars/data-charlist/《义务教育语文课程》（2022年版）识字写字教学基本字表.txt"
OUT   = BASE / "out/scope"

GROUP_SIZE = 50     # 每字约 2 条说法 → 每组约 100 条命令，上限 400


def py_flat(text: str) -> str:
    return " ".join(p[0] for p in pinyin(text, style=Style.NORMAL))


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


def make_phrases(rec, chunk_words) -> list:
    """为一个字生成语音命令词。
    优先「<词>的<字>」（中文里指定汉字的自然说法），
    跳过会产生重复音节的词，再补一条纯词语说法（若组内不歧义）。
    """
    ch = rec["char"]
    for w in rec["words"]:
        p_full = py_flat(f"{w}的{ch}")
        if has_dup_syllable(p_full):
            continue                       # 如「别的的的」，换下一个词
        out = [p_full]
        if len(chunk_words.get(w, [])) == 1:    # 该词在本组内唯一指向这个字
            out.append(py_flat(w))
        return out
    # 所有词都不合适（多为虚词）→ 退化为念单字
    return [py_flat(ch)]


def main():
    recs = json.load(open(CHARS, encoding="utf-8"))
    by_char = {r["char"]: r for r in recs}
    rank = load_freq_rank()

    basic = [c for c in read_charfile(BASIC) if c in by_char]   # 过滤掉偏旁部首
    basic_set = set(basic)
    rest = sorted((r for r in recs if r["char"] not in basic_set),
                  key=lambda r: rank.get(r["char"], 10**9))
    ordered = [by_char[c] for c in basic] + rest
    print(f"排序: 基础字表 {len(basic)} 字（按笔画）+ 其余 {len(rest)} 字（按字频）")

    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.txt"):
        old.unlink()

    n_groups = (len(ordered) + GROUP_SIZE - 1) // GROUP_SIZE
    total_cmds = degraded = 0
    for gi in range(n_groups):
        chunk = ordered[gi * GROUP_SIZE:(gi + 1) * GROUP_SIZE]
        is_basic = (gi + 1) * GROUP_SIZE <= len(basic)

        chunk_words = {}
        for r in chunk:
            for w in r["words"]:
                chunk_words.setdefault(w, []).append(r["char"])

        tag = "基础字" if is_basic else "常用字"
        lines = [
            f"# 学习清单 第 {gi+1} 组（{tag}，第 {gi*GROUP_SIZE+1}-{gi*GROUP_SIZE+len(chunk)} 字）",
            "# 格式: 字id <TAB> 汉字 <TAB> 语音命令词（无声调拼音，多说法用逗号分隔）",
            "# 家长可照此格式自建清单：把课本单元的生字表填进来即可",
        ]
        for r in chunk:
            ph = make_phrases(r, chunk_words)
            if len(ph) == 1 and " " not in ph[0]:
                degraded += 1                       # 退化成单字的
            lines.append(f"{r['id']}\t{r['char']}\t{','.join(ph)}")
            total_cmds += len(ph)

        (OUT / f"{gi+1:02d}_{tag}{gi*GROUP_SIZE+1}-{gi*GROUP_SIZE+len(chunk)}.txt").write_text(
            "\n".join(lines) + "\n", encoding="utf-8")

    print(f"生成 {n_groups} 组 → {OUT}")
    print(f"平均每组 {total_cmds/n_groups:.0f} 条命令词（上限 400）")
    print(f"退化为单字念法的: {degraded} 字（多为虚词，没有合适词组）")
    first = sorted(OUT.glob("*.txt"))[0]
    print(f"\n=== {first.name} ===")
    print("\n".join(first.read_text(encoding="utf-8").splitlines()[:14]))

if __name__ == "__main__":
    main()
