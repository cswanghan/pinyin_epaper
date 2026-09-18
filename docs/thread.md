# 可直接粘贴的短版

长文见 [announce-zh.md](announce-zh.md) / [announce-en.md](announce-en.md)。
下面是配图发推用的短文案，GIF 用 `media/stroke-永.gif`（或 学/好/国）。

---

## 中文 · 开场推（配 stroke-永.gif）

给孩子做了台查字机：**说出任意一个字，屏上就出现它** —— 拼音、组词，
然后一笔一笔写出来，边写边念笔画名。

没有分组没有关卡，就是查字典，只不过是喊的。ESP32-S3，**全程离线**，不联网不上传没账号。

🧵 里面有个反直觉的做法 ↓

## 中文 · 接第二条

动画不自己画笔画 —— 它放的是**成品字自己的像素**。

通常是拿笔画轮廓数据直接渲染动画，但那样动画字形和最终字形是两套东西，
写完那一刻字会跳一下。

反过来做：成品位图是唯一真相，笔画数据只回答「这点属于第几画、排第几」。
于是写完留在屏上的**就是**成品图，一个像素不差，抗锯齿边也原样保留。

## 中文 · 接第三条

靠一条校验强制住：每个字各画像素的并集必须**恰好等于**成品图上全部非黑像素 ——
不缺、不多、不重复。2500 字全过。

顺带记一个坑：这块屏的电源和复位挂在 TCAL9534 IO 扩展器上，厂商 BSP 里没有这段。
不补就是彻底黑屏，而且**一条错误日志都没有**，所有接口都返回成功。

---

## EN · opening post (with stroke-永.gif)

Built my kid a talking dictionary: **say any Chinese character out loud and it appears** —
pinyin, example words, then it's written one stroke at a time while the stroke names are read out.

No levels, no lesson plan. It's a dictionary; you just say the word.
ESP32-S3, **fully offline** — no network, no account.

🧵 One counterintuitive trick inside ↓

## EN · post 2

The animation doesn't draw strokes. It replays **the finished glyph's own pixels**.

Render straight from stroke-outline data and the animated glyph and the displayed glyph are
two different things — so the character *pops* when the last stroke lands.

Inverted it: the finished bitmap is the single source of truth, and the stroke data only
answers *which stroke owns this pixel* and *where in the writing order*. What's left on
screen at the end **is** the finished bitmap, pixel for pixel.

## EN · post 3

Enforced by one offline check: the union of a character's per-stroke pixel lists must be
**exactly** the set of non-black pixels in the finished bitmap. Nothing missing, extra, or
duplicated. All 2500 characters pass.

Bonus trap: this panel's power and reset sit behind a TCAL9534 I/O expander that the vendor
BSP never touches. Skip it and you get a black screen with **zero error logs** — every call
returns success.
