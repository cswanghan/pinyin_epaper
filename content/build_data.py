#!/usr/bin/env python3
"""生成生字数据：字表 → 拼音 + 词组 → chars.json

数据来源:
  字表: 《义务教育语文课程标准（2022年版）》附录5 常用字表一（2500字）
  词组: 《国际中文教育中文水平等级标准》(HSK 2021) 1-9 级词表 —— 按级别挑最简单的词
  补充: mapull 汉语拼音辞典词语表（仅用于 HSK 未覆盖的字）
  拼音: pypinyin
"""
import json, sys
from pathlib import Path
from collections import defaultdict

BASE = Path(__file__).parent
D = BASE / "data"
CHARLIST = D / "hanzi-chars/data-charlist/《义务教育语文课程》（2022年版）常用字表一.txt"
HSK      = D / "hanzi-words/words/HSK词汇（新版共九级）2021.txt"
MAPULL   = D / "hanzi-words/words/mapull-词语.txt"
OUT      = BASE / "out/chars.json"

N_WORDS = 3          # 每字词组数 —— 200x200 屏幕放不下更多
LEVEL_NUM = {f"{n}级": i for i, n in enumerate(
    "一 二 三 四 五 六 七 八 九".split(), start=1)}


def load_charlist(path: Path) -> list[str]:
    """跳过 # 注释行（表头 + 音序分组标记），按原顺序提取汉字。"""
    seen, chars = set(), []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.lstrip().startswith("#"):
            continue
        for c in line:
            if "一" <= c <= "鿿" and c not in seen:
                seen.add(c); chars.append(c)
    return chars


def load_hsk(charset: set[str]):
    """HSK 词表 → {字: [(级别, 是否双字, 序号, 词)]}，全部组成字须在 charset 内。"""
    by_char = defaultdict(list)
    for line in HSK.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        col = line.split("\t")
        if len(col) < 5:
            continue
        word, _pos, _py, seq, level = col[0], col[1], col[2], col[3], col[4]
        # "爸爸∣爸" 这类变体取第一个
        word = word.split("∣")[0].strip()
        if not (2 <= len(word) <= 3):
            continue
        in_set = all(c in charset for c in word)
        lv = LEVEL_NUM.get(level.strip(), 99)
        try: seq_n = int(seq)
        except ValueError: seq_n = 99999
        for c in set(word):
            # 排序键：优先组成字全在表内 → 级别低 → 双字 → 序号小
            by_char[c].append((not in_set, lv, len(word) != 2, seq_n, word))
    for c in by_char:
        by_char[c].sort()          # 级别低 → 双字优先 → 序号小
    return by_char


def load_mapull(charset: set[str], need: set[str]):
    """兜底词源：只为 HSK 未覆盖的字取词，按词长排序（短词通常更常用）。"""
    by_char = defaultdict(list)
    if not MAPULL.exists() or not need:
        return by_char
    for line in MAPULL.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        word = line.split("\t")[0].strip()
        if not (2 <= len(word) <= 3):
            continue
        hit = need & set(word)
        if not hit:
            continue
        in_set = all(c in charset for c in word)
        for c in hit:
            by_char[c].append((not in_set, len(word) != 2, word))
    for c in by_char:
        by_char[c].sort()
    return by_char


def main():
    for p in (CHARLIST, HSK):
        if not p.exists():
            sys.exit(f"缺少数据文件: {p}")

    chars = load_charlist(CHARLIST)
    charset = set(chars)
    print(f"字表: {len(chars)} 字")

    hsk = load_hsk(charset)
    print(f"HSK 覆盖: {len(hsk)} 字")

    need = {c for c in charset if not hsk.get(c)}
    extra = load_mapull(charset, need)
    print(f"兜底词源补充: {len(extra)} 字（HSK 未覆盖 {len(need)} 字）")

    # 人工覆盖表：修正 HSK/兜底词源对数字、虚词给出的不当选词
    OVERRIDE = {}
    ov_path = D / "word_overrides.txt"
    if ov_path.exists():
        for line in ov_path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split()          # 制表符/空格都按空白切
            if len(parts) >= 2:
                OVERRIDE[parts[0]] = parts[1:1 + N_WORDS]
        print(f"人工覆盖 {len(OVERRIDE)} 字")

    from pypinyin import pinyin, Style
    records, no_word = [], []
    for idx, c in enumerate(chars):
        # 主读音用 pypinyin 默认模式（最常用读音），不用 heteronym —— 后者会带进生僻音
        primary = pinyin(c, style=Style.TONE)[0][0]

        if c in OVERRIDE:
            words, src = OVERRIDE[c], "manual"
        else:
            words = [w for *_, w in hsk.get(c, [])[:N_WORDS]]
            src = "hsk"
        if not words and src != "manual":
            # 兜底词源无词频信息，只取 1 个，避免"蝙蝠扇"这类生造词
            fill = [w for *_, w in extra.get(c, [])][:1]
            if fill:
                words, src = fill, "extra"
        if not words:
            no_word.append(c); src = "none"

        # 从选中词组里观察该字的实际读音，数据驱动地发现真多音字
        readings = {pinyin(w, style=Style.TONE)[w.index(c)][0] for w in words if c in w}
        readings.add(primary)

        records.append({
            "id": idx, "char": c, "pinyin": primary,
            "readings": sorted(readings),   # 在所选词组中实际出现的读音
            "words": words, "word_src": src,
        })

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding="utf-8")

    full = sum(1 for r in records if len(r["words"]) == N_WORDS)
    multi = sum(1 for r in records if len(r["readings"]) > 1)
    print(f"\n已写入 {OUT}")
    print(f"  共 {len(records)} 字，{full} 字有满 {N_WORDS} 个词组")
    print(f"  无词组: {len(no_word)} 字" + (f" → {''.join(no_word)}" if no_word else ""))
    print(f"  词组中出现多读音的字: {multi}")
    print("\n抽样:")
    for i in (0, 1, 100, 500, 1200, 2000, 2499):
        r = records[i]
        print(f"  {r['char']}  {r['pinyin']:7s} {' '.join(r['words']):20s} [{r['word_src']}]")

if __name__ == "__main__":
    main()
