# I built my kid a talking dictionary that writes the character for you

![four finished screens](media/hero.png)

The question a kid actually asks is *"how do you write that one?"* — and a paper dictionary
needs you to already know the radical, while a phone stops being a dictionary the moment it's out.

So: **say any character out loud and it appears** — pinyin, example words, then it's written
one stroke at a time while the stroke names are read aloud.

No levels, no lesson plan, no grouping. It's a dictionary. You just say the word.

---

## What it looks like

| 永 | 学 |
|---|---|
| ![yong](media/stroke-永.gif) | ![xue](media/stroke-学.gif) |

The stroke currently being written is warm yellow; it turns white the instant it's done.
Behind it is the 米字格 grid from Chinese school practice books. While the device says
*héng-zhé-gōu*, that stroke is growing — then it pauses 600 ms, long enough for a kid to
copy it on paper.

> Both GIFs are **pixel-exact renders of the real device data**, not mockups — the input is
> literally the two files burned onto the SD card.

---

## Three decisions that make it feel right

### 1. The animation doesn't draw strokes. It replays the finished glyph's own pixels.

The obvious approach is to render the animation straight from stroke-outline data
(makemeahanzi or similar). The problem: the animated glyph and the displayed glyph come
from two different sources, so the character **pops** the moment the last stroke lands.

I inverted it. The final bitmap on screen (368×448, rendered from a real typeface) is the
single source of truth. The stroke data only answers two questions: *which stroke does this
ink pixel belong to*, and *where in that stroke's writing order*. The animation just releases
the glyph's own pixels in order.

So what's left on screen at the end **is** the finished bitmap, pixel for pixel. Antialiased
edges survive intact — no crunchy stroke borders. The cost is that a few pixels may get
assigned to a neighbouring stroke, which is invisible at this size.

An offline check enforces the invariant: the union of a character's per-stroke pixel lists
must be **exactly** the set of non-black pixels in the finished bitmap — nothing missing,
nothing extra, nothing duplicated. All 2500 characters pass.

### 2. Audio and animation must start on the same instant

Audio is a queue plus a worker task. The naive hook is "start drawing when the clip is
queued" — but at that moment the previous clip is usually still playing, so the drawing runs
ahead of the sound, and the drift compounds stroke after stroke.

Instead: before queueing stroke *i*'s clip, poll until the audio queue is empty, *then*
queue, *then* start drawing. Now "queued" and "starts sounding" are the same instant, and the
duration handed to the animation is real. Stroke 1 waits out the character + example words;
every later stroke waits out the previous stroke's pause. Zero changes to the audio module.

### 3. Push only the rows that changed

A full-screen push measures 21–29 ms. Each animation frame only touches a few rows inside the
character box — pushed by row, that's **1–2 ms**.

---

## Hardware & data

- **ESP32-S3 + 368×448 AMOLED** (Waveshare ESP32-S3-Touch-AMOLED-1.8). Touch: tap to wake,
  swipe to page through characters
- **Fully offline.** WakeNet wake word + MultiNet command recognition run on-chip. No network,
  no upload, no account, no subscription
- **2500 characters × 14246 spoken forms** — six ways to say each one (disambiguating
  homophones the way Chinese speakers actually do), bucketed into 387 groups by final
  syllable; the whole table builds in 91 ms
- **1.1 GB on the SD card**: per character, a 368×448 RGB565 bitmap, the character's audio,
  an example-word clip, and the stroke animation data
- Stroke animation: 62 MB in a compact custom format, up to 23 strokes
- One card feeds two different boards (there's also a 200×200 four-colour e-paper build);
  the firmware switches at compile time

---

## Traps, for anyone building on this panel

- **The panel's power and reset aren't on GPIOs — they're behind a TCAL9534 I/O expander.**
  Vendor BSP v2.0.3 doesn't touch it. Skip it and you get a completely black screen with
  **no error log at all**: every call returns success.
- **A full-frame PSRAM buffer can't go to `draw_bitmap` in one call.** One SPI transaction
  caps at 32 KB, and PSRAM addresses aren't DMA-capable. Band it 32 rows at a time.
- **makemeahanzi's y axis points up** (its SVG carries `scale(1,-1)`). Use it raw and stroke
  order comes out inverted — 三 writes bottom to top. Because my glyph comes from the finished
  bitmap, the bug shows up as *wrong stroke order* rather than an upside-down character, which
  is considerably harder to spot.
- **jsdelivr returns an error string with exit code 0 for files over 20 MB.** Use
  raw.githubusercontent.com.
- **AMOLED doesn't retain its image across power-off** (e-paper does), so the "what's already
  on screen" state in NVS has to be cleared at boot — otherwise the first refresh is skipped
  and you boot to a blank panel.

---

## Not done yet

Power button and battery gauge move to the AXP2101; fonts move into flash, which drops the
786 MB of bitmaps from the SD card.
