#!/usr/bin/env python3
"""生成朗读音频：每字 2 段 —— 字音 + 词组连读

输出 out/aud/<id>_c.wav（字音）和 <id>_w.wav（词组）
格式: 16kHz / 16bit / 单声道 PCM —— ESP32 读了直接推 I2S，无需解码

特性: 支持断点续跑（已存在的文件跳过）、失败重试、并发限流
"""
import asyncio, json, subprocess, sys, shutil
from pathlib import Path

BASE = Path(__file__).parent
CHARS = BASE / "out/chars.json"
AUD   = BASE / "out/aud"
TMP   = BASE / "out/.tts_tmp"

VOICE   = "zh-CN-XiaoxiaoNeural"
RATE    = "-10%"          # 略慢，便于孩子听清；比 -20% 自然
CONCUR  = 8               # 并发数，太高会被限流
RETRY   = 3

async def tts(text: str, out_mp3: Path) -> bool:
    import edge_tts
    for attempt in range(RETRY):
        try:
            c = edge_tts.Communicate(text, VOICE, rate=RATE)
            await c.save(str(out_mp3))
            if out_mp3.exists() and out_mp3.stat().st_size > 500:
                return True
        except Exception as e:
            if attempt == RETRY - 1:
                print(f"  ✗ {text[:12]}: {e}", file=sys.stderr)
            await asyncio.sleep(1.5 * (attempt + 1))
    return False

def to_wav(mp3: Path, wav: Path) -> bool:
    """转 16k/16bit/单声道 PCM。"""
    r = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(mp3),
         "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", str(wav)],
        capture_output=True)
    return r.returncode == 0 and wav.exists()

async def one(rec, sem, stats):
    async with sem:
        for kind, text in (("c", rec["char"]),
                           ("w", "，".join(rec["words"]) + "。" if rec["words"] else None)):
            if not text:
                continue
            wav = AUD / f"{rec['id']:04d}_{kind}.wav"
            if wav.exists() and wav.stat().st_size > 1000:
                stats["skip"] += 1
                continue
            mp3 = TMP / f"{rec['id']:04d}_{kind}.mp3"
            if await tts(text, mp3) and to_wav(mp3, wav):
                stats["ok"] += 1
                mp3.unlink(missing_ok=True)
            else:
                stats["fail"] += 1
                print(f"  ✗ id={rec['id']} {rec['char']} [{kind}]", file=sys.stderr)

async def main():
    if not shutil.which("ffmpeg"):
        sys.exit("需要 ffmpeg：brew install ffmpeg")
    recs = json.load(open(CHARS, encoding="utf-8"))
    if len(sys.argv) > 1 and sys.argv[1] == "--sample":
        recs = recs[:int(sys.argv[2]) if len(sys.argv) > 2 else 5]
    AUD.mkdir(parents=True, exist_ok=True)
    TMP.mkdir(parents=True, exist_ok=True)

    sem = asyncio.Semaphore(CONCUR)
    stats = {"ok": 0, "fail": 0, "skip": 0}
    tasks = [one(r, sem, stats) for r in recs]
    done = 0
    for chunk_start in range(0, len(tasks), 100):
        await asyncio.gather(*tasks[chunk_start:chunk_start + 100])
        done = min(chunk_start + 100, len(tasks))
        print(f"  {done}/{len(recs)} 字  生成{stats['ok']} 跳过{stats['skip']} 失败{stats['fail']}", flush=True)

    print(f"\n完成: 生成 {stats['ok']}, 跳过 {stats['skip']}, 失败 {stats['fail']}")
    print(f"输出 → {AUD}  （失败的重跑本脚本即可续传）")

if __name__ == "__main__":
    asyncio.run(main())
