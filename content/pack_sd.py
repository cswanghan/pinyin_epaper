#!/usr/bin/env python3
"""把生成的内容打包到 SD 卡

用法: python pack_sd.py /Volumes/<SD卡名>
目标结构:
  <SD>/pinyin/index.txt        生字总数
  <SD>/pinyin/chars.json       元数据（固件暂不读，便于排查）
  <SD>/pinyin/img/0000.bin     墨水屏图，10000 字节
  <SD>/pinyin/aud/0000_c.wav   字音
  <SD>/pinyin/aud/0000_w.wav   词组
  <SD>/pinyin/scope/*.txt      学习清单（一组一个文件）
  <SD>/pinyin/sys/*.wav        语音提示（gen_prompts.py 生成，缺了固件用蜂鸣音代替）
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
SRC_AUD = BASE / "out/aud"
CHARS   = BASE / "out/chars.json"

DEFAULT_CONFIG = """\
# 小学生字学习机配置 —— 改完重启设备生效，# 后面是注释
# 音量 0-100
volume=80
# 唤醒灵敏度: normal 正常 / high 更灵敏（离得远也能唤醒，但误唤醒会多）
wake_sensitivity=normal
# 命令词识别阈值 0-1，0 = 模型默认。常把别的话听成命令就调高（如 0.3），说了没反应就调低（如 0.1）
mn_threshold=0
# 唤醒后等命令的秒数（2-30）
listen_seconds=6
# 执行完一条命令后继续听的秒数，期间直接说下一条命令不用再唤醒；0 = 关闭（0-30）
follow_up_seconds=6
# 用电池时多少分钟没操作自动关机，0 = 不自动关机（插着电脑时不会自动关机）
sleep_minutes=10
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
