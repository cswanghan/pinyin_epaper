#!/usr/bin/env python3
"""把生成的内容打包到 SD 卡

用法: python pack_sd.py /Volumes/<SD卡名>
目标结构:
  <SD>/pinyin/index.txt        生字总数
  <SD>/pinyin/chars.json       元数据（固件暂不读，便于排查）
  <SD>/pinyin/img/0000.bin     墨水屏图，200x200 四色，10000 字节
  <SD>/pinyin/img565/0000.bin  AMOLED 图，368x448 RGB565 大端，329728 字节（约 786 MB）
  <SD>/pinyin/anim/0000.bin    笔顺动画（只 AMOLED 用），PSA1 格式，5~35 KB 不等（约 62 MB）
  <SD>/pinyin/aud/0000_c.wav   字音
  <SD>/pinyin/aud/0000_w.wav   词组
  <SD>/pinyin/scope/*.txt      学习清单（一组一个文件）
  <SD>/pinyin/sys/*.wav        语音提示（gen_prompts.py 生成，缺了固件用蜂鸣音代替）
  <SD>/pinyin/stroke/bishun.txt  笔顺表（gen_stroke.py 生成，缺了就不念笔顺）
  <SD>/pinyin/stroke/*.wav       笔画名等音频片段
  <SD>/pinyin/config.txt       设备配置（已存在则不覆盖，保留家长改过的值）
  <SD>/pinyin/progress.txt     学习进度（固件写，只读查看）
"""
import json, os, shutil, subprocess, sys
from pathlib import Path

# macOS 在 FAT32 上会为每个文件生成 ._xxx 的 AppleDouble 附属文件（存扩展属性），
# 导致文件数翻倍、浪费空间。置此环境变量可禁止生成。
os.environ["COPYFILE_DISABLE"] = "1"

BASE = Path(__file__).parent
SRC_IMG = BASE / "out/img"
SRC_IMG565 = BASE / "out/img565"        # AMOLED 板的图，没生成就跳过
IMG565_BYTES = 368 * 448 * 2            # 329728
SRC_ANIM = BASE / "out/anim"            # AMOLED 板的笔顺动画，没生成就跳过
SRC_AUD = BASE / "out/aud"
CHARS   = BASE / "out/chars.json"

DEFAULT_CONFIG = """\
# 小学生字学习机配置 —— 改完重启设备生效，# 后面是注释
# 音量 0-100
volume=100
# 唤醒灵敏度: high 灵敏（默认）/ normal 误唤醒少一些，但离得远、声音小时不容易叫醒
wake_sensitivity=high
# 命令词识别阈值 0-1，0 = 模型默认。常把别的话听成命令就调高（如 0.3），说了没反应就调低（如 0.1）
mn_threshold=0
# 唤醒后等命令的秒数（2-30）
listen_seconds=6
# 执行完一条命令后继续听的秒数，期间直接说下一条命令不用再唤醒；0 = 关闭（0-30）
follow_up_seconds=6
# 用电池时多少分钟没操作自动关机，0 = 不自动关机（插着电脑时不会自动关机）
sleep_minutes=10
# 刷屏时念笔顺 on/off。刷一次屏要 17.6 秒，念笔顺正好把这段等待填上：
# 孩子跟着读音和笔顺在纸上写，写完抬头字刚好出现
stroke_order=on
# 笔画之间停顿多少毫秒。0 = 自动：按这个字的笔画数摊，念完刚好赶上刷屏结束。
# 觉得太赶就填个固定值（如 800），但笔画多的字会念到字出来之后
stroke_gap_ms=0
"""

def main():
    if len(sys.argv) < 2:
        sys.exit("用法: python pack_sd.py /Volumes/<SD卡名>")
    dst_root = Path(sys.argv[1])
    if not dst_root.exists():
        sys.exit(f"目标不存在: {dst_root}")

    recs = json.load(open(CHARS, encoding="utf-8"))
    dst = dst_root / "pinyin"
    (dst / "img").mkdir(parents=True, exist_ok=True)
    (dst / "aud").mkdir(parents=True, exist_ok=True)

    # 只统计图片齐全的字 —— 图片是显示的前提，音频缺失可降级
    missing_img, missing_aud = [], []
    n_img = n_aud = 0
    for r in recs:
        i = r["id"]
        src = SRC_IMG / f"{i:04d}.bin"
        if src.exists() and src.stat().st_size == 10000:
            shutil.copy(src, dst / "img" / src.name); n_img += 1
        else:
            missing_img.append(i)
        for kind in ("c", "w"):
            a = SRC_AUD / f"{i:04d}_{kind}.wav"
            if a.exists():
                shutil.copy(a, dst / "aud" / a.name); n_aud += 1
            elif kind == "c" or r["words"]:
                missing_aud.append(a.name)

    # AMOLED 彩屏图。一张卡要同时喂两块板子，所以和墨水屏的 img/ 并排放，互不覆盖。
    # 786 MB 拷一次要好几分钟，已经在卡上且大小对的就跳过，重跑不用重来。
    n565 = skip565 = 0
    if SRC_IMG565.exists():
        d565 = dst / "img565"
        d565.mkdir(exist_ok=True)
        for r in recs:
            src = SRC_IMG565 / f"{r['id']:04d}.bin"
            if not (src.exists() and src.stat().st_size == IMG565_BYTES):
                continue
            tgt = d565 / src.name
            if tgt.exists() and tgt.stat().st_size == IMG565_BYTES:
                skip565 += 1
                continue
            shutil.copy(src, tgt); n565 += 1
            if n565 % 200 == 0:
                print(f"  彩屏图 {n565} 张…", flush=True)
        # COPYFILE_DISABLE 挡得住 cp/tar，挡不住 shutil 走的 fcopyfile —— 它会把
        # macOS 给新文件加的扩展属性一起拷过去，在 FAT 上就落成 2500 个 ._xxx 附属文件。
        # 实测确实会出现，所以拷完统一清一遍。
        junk = 0
        for f in d565.glob("._*"):
            f.unlink(); junk += 1
        print(f"彩屏图 {n565 + skip565}/{len(recs)} 张（新拷 {n565}，已存在 {skip565}）"
              + (f"，清掉 {junk} 个 ._ 附属文件" if junk else ""))

    # 笔顺动画（只有 AMOLED 板用）。大小不固定，用「非空且和源一样大」当已拷过的判据
    na = skipa = 0
    if SRC_ANIM.exists():
        da = dst / "anim"
        da.mkdir(exist_ok=True)
        for r in recs:
            src = SRC_ANIM / f"{r['id']:04d}.bin"
            if not src.exists():
                continue
            tgt = da / src.name
            if tgt.exists() and tgt.stat().st_size == src.stat().st_size:
                skipa += 1
                continue
            shutil.copy(src, tgt); na += 1
            if na % 400 == 0:
                print(f"  笔顺动画 {na} 份…", flush=True)
        junk = 0                      # 同上，shutil 在 FAT 上会留 ._ 附属文件
        for f in da.glob("._*"):
            f.unlink(); junk += 1
        print(f"笔顺动画 {na + skipa}/{len(recs)} 份（新拷 {na}，已存在 {skipa}）"
              + (f"，清掉 {junk} 个 ._ 附属文件" if junk else ""))

    # 学习清单（语音跳转的命令词来源，家长也可自行增删）
    src_scope = BASE / "out/scope"
    n_scope = 0
    if src_scope.exists():
        (dst / "scope").mkdir(exist_ok=True)
        for f in sorted(src_scope.glob("*.txt")):
            shutil.copy(f, dst / "scope" / f.name); n_scope += 1
    print(f"学习清单 {n_scope} 组")

    # 语音提示（可选）
    src_sys = BASE / "out/sys"
    n_sys = 0
    if src_sys.exists():
        (dst / "sys").mkdir(exist_ok=True)
        for f in sorted(src_sys.glob("*.wav")):
            shutil.copy(f, dst / "sys" / f.name); n_sys += 1
    print(f"语音提示 {n_sys} 条" + ("" if n_sys else "（没有 out/sys，先跑 gen_prompts.py；不跑也能用，固件用蜂鸣音代替）"))

    # 笔顺表 + 笔画名片段（可选）
    src_stroke = BASE / "out/stroke"
    n_stroke = 0
    if src_stroke.exists():
        (dst / "stroke").mkdir(exist_ok=True)
        for f in sorted(src_stroke.iterdir()):
            if f.suffix in (".wav", ".txt"):
                shutil.copy(f, dst / "stroke" / f.name); n_stroke += 1
    print(f"笔顺片段 {n_stroke} 个" + ("" if n_stroke else "（没有 out/stroke，先跑 gen_stroke.py；不跑也能用，只是刷屏时不念笔顺）"))

    cfg = dst / "config.txt"
    if not cfg.exists():
        cfg.write_text(DEFAULT_CONFIG, encoding="utf-8")
        print("写入默认 config.txt")

    (dst / "index.txt").write_text(f"{len(recs)}\n", encoding="utf-8")
    shutil.copy(CHARS, dst / "chars.json")

    # 清理可能残留的 AppleDouble 文件
    subprocess.run(["dot_clean", "-m", str(dst_root)], capture_output=True)
    for junk in dst.rglob("._*"):
        junk.unlink(missing_ok=True)

    total = sum(f.stat().st_size for f in dst.rglob("*") if f.is_file())
    print(f"图片 {n_img}/{len(recs)}   音频 {n_aud}   共 {total/1024/1024:.1f} MB → {dst}")
    if missing_img:
        print(f"⚠ 缺图 {len(missing_img)} 个: {missing_img[:10]}")
    if missing_aud:
        print(f"⚠ 缺音频 {len(missing_aud)} 个（固件会跳过播放）: {missing_aud[:6]}")

if __name__ == "__main__":
    main()
