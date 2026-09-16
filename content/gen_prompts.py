#!/usr/bin/env python3
"""生成固件用的语音提示 → out/sys/<key>.wav

固件播放提示时先找 SD 卡上的 pinyin/sys/<key>.wav，找不到就退回蜂鸣提示音，
所以这些文件都是可选的。wake / timeout 默认只用提示音（短、不打扰），
想换成人声可以自己放 wake.wav / timeout.wav（16kHz 单声道 PCM）。

用法: .venv/bin/python gen_prompts.py [--force]    --force 覆盖已生成的文件（改了文案时用）
"""
import asyncio, sys
from pathlib import Path

from gen_audio import tts, to_wav, cn_num, TRIM

BASE = Path(__file__).parent
OUT  = BASE / "out/sys"
TMP  = BASE / "out/.tts_tmp"
MAX_GROUPS = 99          # 与固件 GROUPS_MAX 一致（切组提示，控制台 g N 用）

PROMPTS = {
    "busy":     "等一下哦",              # 刷屏中说了翻页类命令
    "mastered": "真棒！",                # 我会了
    "forgot":   "好的，再学一遍",        # 忘了
    "all_done": "这一组都会了，真棒！",  # 本组没有未掌握的字
    "bye":      "再见",                  # 关机
    "not_found": "没听清，再说一遍",     # 听到「…的X」但对不上是哪个字
}

async def gen(key, text, sem, force, stats):
    wav = OUT / f"{key}.wav"
    if wav.exists() and not force:
        stats["skip"] += 1
        return
    async with sem:
        mp3 = TMP / f"sys_{key}.mp3"
        ok = await tts(text, mp3) and to_wav(mp3, wav, af=TRIM)
        mp3.unlink(missing_ok=True)
    stats["ok" if ok else "fail"] += 1

async def main() -> bool:
    force = "--force" in sys.argv
    OUT.mkdir(parents=True, exist_ok=True)
    TMP.mkdir(parents=True, exist_ok=True)
    items = dict(PROMPTS)
    for n in range(1, MAX_GROUPS + 1):
        items[f"group_{n:02d}"] = f"第{cn_num(n)}组"

    sem = asyncio.Semaphore(8)
    stats = {"ok": 0, "skip": 0, "fail": 0}
    await asyncio.gather(*(gen(k, t, sem, force, stats) for k, t in items.items()))
    print(f"语音提示 {len(items)} 条: 新生成 {stats['ok']}  已存在 {stats['skip']}  失败 {stats['fail']} → {OUT}")
    return stats["fail"] == 0

if __name__ == "__main__":
    sys.exit(0 if asyncio.run(main()) else 1)
