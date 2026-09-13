# 小学生字学习机

基于 Waveshare **ESP32-S3-ePaper-1.54G** 的离线生字学习设备：墨水屏展示生字（拼音 + 楷体大字 + 词组），语音交互。

## 硬件

| 项目 | 规格 |
|---|---|
| 主控 | ESP32-S3-PICO-1 (LGA56) rev v0.2，240MHz 双核 |
| 存储 | 内置 8MB Flash + 8MB PSRAM |
| 屏幕 | 1.54" 四色墨水屏 200×200（红/黄/黑/白）|
| 刷新 | **全局 20 秒 / 快速 15 秒** ← 决定了整个交互设计 |
| 音频 | ES8311 编解码 + 板载麦克风 + 板载扬声器 + PA |
| 其他 | Micro SD（FAT32）、RTC PCF85063、温湿度 SHTC3、锂电池管理 |
| MAC | `70:04:1d:d7:eb:04` |

### 进入下载模式（重要）

板子**只有 BOOT 和 PWR 键，没有 RST 键**。官方方法是「按住 BOOT，重新上电」：

```
拔 USB（接了电池要一起拔）→ 按住 BOOT → 插 USB → 保持 2-3 秒 → 松开
```

平时插电脑**不被识别是正常的**：S3 用原生 USB（直连 GPIO19/20），必须芯片在运行才枚举，
而墨水屏固件跑起来就 deep sleep，USB 随之断电。

```bash
esptool --port /dev/cu.usbmodem* --after no-reset flash-id   # --after no-reset 可保持端口不掉
```

## 设计要点

**墨水屏刷新要 15-20 秒**，所以：屏幕只做静态展示，**全部交互靠音频**。
切字时刷屏与朗读并行，把等待掩盖掉。这利用了墨水屏的长处（持久显示、零功耗、护眼），
避开了它的短处。

**图片格式**：直接生成显示驱动的原生缓冲（取自 `GUI_Paint.c` 的 `Scale==4` 分支），
4 像素/字节、高位在前、行主序，每张 **10,000 字节**。固件 `fread` 完直接 `epaper_port_display()`，
零转换。

> 不使用官方 `GUI_ReadBmp_RGB_4Color()`：它对奇数像素的取位有 bug
> （`temp = byte >> 0` 后 `temp >> 2` 会混入相邻像素的位）。

**音频格式**：16kHz / 16bit / **单声道** PCM WAV。codec 按双声道打开，
固件里实时把单声道复制成双声道 —— 用一点 CPU 换一半 SD 空间（335MB vs 670MB）。

## 目录结构

```
content/                内容生成管线（PC 端 Python）
  build_data.py         字表 → 拼音 + 词组 → out/chars.json
  gen_images.py         → out/img/*.bin（设备格式）+ out/preview/*.png
  gen_audio.py          → out/aud/*.wav（支持断点续传）
  pack_sd.py            → 组装 SD 卡目录
  data/                 字表与词表数据源（git clone 来的）
  fonts/                霞鹜文楷（开源楷体，小学课本字形）
firmware/pinyin_epaper/ ESP-IDF 工程（基于官方 09_E_Paper_Test + 07_Audio_Test 组件）
vendor/waveshare/       官方例程与资料（参考用）
```

## 数据来源

| 内容 | 来源 |
|---|---|
| 字表 | 《义务教育语文课程标准（2022年版）》附录5 常用字表一，**2500 字** |
| 词组 | 《国际中文教育中文水平等级标准》(HSK 2021) 1-9 级词表，**按级别取最简单的词** |
| 兜底词 | mapull 汉语拼音辞典（仅用于 HSK 未覆盖的字，每字只取 1 个避免噪声）|
| 拼音 | pypinyin（主读音用默认模式；多音字从所选词组中数据驱动地发现）|
| 字体 | 霞鹜文楷 LXGW WenKai（SIL OFL）|
| 朗读 | edge-tts `zh-CN-XiaoxiaoNeural`，语速 -10% |

覆盖率：2499/2500 字有词组（仅「俺」无合适双字词）。

## 内容生成

```bash
cd content
uv venv --python 3.11 .venv
uv pip install --python .venv/bin/python pypinyin jieba pillow edge-tts

.venv/bin/python build_data.py        # → out/chars.json
.venv/bin/python gen_images.py        # → 2500 张图（约 2 分钟）
.venv/bin/python gen_audio.py         # → 5000 段音频（约 80 分钟，可断点续传）
.venv/bin/python pack_sd.py /Volumes/<SD卡名>
```

`gen_images.py` 可传汉字只生成样张：`gen_images.py 水 服 便`

## SD 卡结构

```
<SD>/pinyin/index.txt        生字总数
<SD>/pinyin/chars.json       元数据（固件不读，便于排查）
<SD>/pinyin/img/0000.bin     10000 字节
<SD>/pinyin/aud/0000_c.wav   字音
<SD>/pinyin/aud/0000_w.wav   词组连读
```

## 固件编译

```bash
. ~/esp/esp-idf/export.sh          # ESP-IDF v5.5.1
cd firmware/pinyin_epaper
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

分区表按 **8MB flash** 排布：4MB 应用 + 3.9MB 预留给 esp-sr 语音模型。

## 当前交互（按键版）

语音识别接入前先用按键验证链路：

| 操作 | 行为 |
|---|---|
| 单击 BOOT | 下一个生字 |
| 双击 BOOT | 重播字音 |
| 长按 BOOT | 播放词组 |

## 路线图

- [x] M1 内容管线（字表 / 图片 / 音频 / SD 打包）
- [x] M2 固件骨架（SD 读图刷屏 + WAV 播放 + 按键）
- [ ] M3 离线语音（esp-sr：WakeNet 唤醒 + MultiNet 中文命令词）
- [ ] M4 应用逻辑（学习进度、复习模式、电池休眠）

### M3 备注

唤醒词只能用乐鑫**预训练**的（「你好小智」「Hi 乐鑫」等），自定义需向乐鑫申请训练。
命令词（MultiNet）可自由定义中文短语，不受此限制。

计划命令集：下一个 / 上一个 / 再读一遍 / 读词组 / 我会了 / 复习。
