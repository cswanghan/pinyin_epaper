#!/usr/bin/env python3
"""生成固件用的语音提示 → out/sys/<key>.wav

固件播放提示时先找 SD 卡上的 pinyin/sys/<key>.wav，找不到就退回蜂鸣提示音，
所以这些文件都是可选的。wake / timeout 默认只用提示音（短、不打扰），
想换成人声可以自己放 wake.wav / timeout.wav（16kHz 单声道 PCM）。

用法: .venv/bin/python gen_prompts.py [--force]    --force 覆盖已生成的文件（改了文案时用）
"""
import asyncio, sys
from pathlib import Path

from gen_audio import tts, to_wav

BASE = Path(__file__).parent
OUT  = BASE / "out/sys"
TMP  = BASE / "out/.tts_tmp"
MAX_GROUPS = 99          # 与固件 SR_MAX_GROUPS 一致

# edge-tts 输出前面约 0.2 秒、后面约 1 秒静音。固件播放期间给语音识别喂静音（防自激），
# 尾巴上的静音等于白白多「聋」1 秒，所以首尾裁掉，只留 0.05 / 0.15 秒
TRIM = ("silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.05,areverse,"
        "silenceremove=start_periods=1:start_threshold=-50dB:start_silence=0.15,areverse")

PROMPTS = {
    "busy":     "等一下哦",              # 刷屏中说了翻页类命令
    "mastered": "真棒！",                # 我会了
    "forgot":   "好的，再学一遍",        # 忘了
    "all_done": "这一组都会了，真棒！",  # 本组没有未掌握的字
    "bye":      "再见",                  # 关机
}

CN = "零一二三四五六七八九"

def cn_num(n: int) -> str:
    """1..99 → 中文数字，交给 TTS 读更稳定"""
    if n < 10:
        return CN[n]
    tens, ones = divmod(n, 10)
    return ("" if tens == 1 else CN[tens]) + "十" + ("" if ones == 0 else CN[ones])

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
