/*****************************************************************************
 * 小学生字学习机 —— 主应用
 *
 * 硬件: 由 main/user_config.h 的 APP_BOARD 选定
 *   BOARD_EPAPER_1_54  Waveshare ESP32-S3-ePaper-1.54G (200x200 四色墨水屏 + ES8311 + SD)
 *   BOARD_AMOLED_1_8   Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 AMOLED + ES8311 + SD)
 *                      P0 阶段只搬语音，屏还没接，display_task 退化成「只念笔顺」
 *
 * 任务划分:
 *   cmd_task      唯一持有学习状态（当前组 / 当前字 / 掌握标记）。语音、按键、
 *                 控制台都只往 s_cmd_q 投递命令，由它串行处理 —— 不用加锁
 *   display_task  刷屏（阻塞约 20 秒，面板波形掐不断）。刷屏期间命令照常执行:
 *                 换字排进深度 1 的显示队列（后者覆盖前者），这一张刷完直接刷最新的
 *   audio_task    朗读与提示音（app_audio.cpp），与刷屏并行，掩盖等待
 *   sr_*          离线语音（app_sr.cpp）
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "user_app.h"
#include "user_config.h"
#include "button_bsp.h"
#include "audio_bsp.h"
#include "i2c_bsp.h"
#include "sdcard_bsp.h"
#if BOARD_HAS_EPAPER
#include "epaper_port.h"
#endif
#if BOARD_HAS_AMOLED
#include "amoled_port.h"
/* 点亮验收开关。开机刷一张校验图，用来确认 RGB565 字节序、原点方向、X 偏移。
 * 三样已于 2026-09-18 在实机确认无误，平时保持 0；改了送屏相关代码再临时打开。*/
#define AMOLED_TEST_PATTERN  0
#define AMOLED_BRIGHTNESS    80
#endif
#include "app_sr.h"
#include "app_audio.h"
#include "app_store.h"
#include "app_stroke.h"
#include "app_power.h"
#include "app_console.h"
#if BOARD_HAS_TOUCH
#include "touch_bsp.h"
#endif

#define TAG "PINYIN"

#define EPD_W          200
#define EPD_H          200
#define EPD_BUF_SIZE   ((EPD_W / 4) * EPD_H)      /* 4 像素/字节 = 10000 */

#define SD_ROOT        "/sdcard/pinyin"
#define SCOPE_DIR      SD_ROOT "/scope"
#define CONFIG_PATH    SD_ROOT "/config.txt"
#define PROGRESS_PATH  SD_ROOT "/progress.txt"

#define EXPORT_DELAY_US     (30LL * 1000 * 1000)  /* 进度变化后空闲 30 秒再导出 */

/* 按键事件位（button_bsp.c 的回调里定义，与 multi_button 的枚举值无关）*/
#define BOOT_SINGLE  0
#define BOOT_DOUBLE  1
#define BOOT_LONG    3
#define PWR_SINGLE   0
#define PWR_DOUBLE   1
#define PWR_LONG     2

#define EVT_SR_READY  (-20)   /* 语音模型加载完，可以注册命令词了 */

/* ---------- 状态（只在 cmd_task 里读写；初始化阶段除外）---------- */
static app_config_t s_cfg;
static int      s_total      = 0;
static scope_t *s_scope      = NULL;    /* 当前学习组 */
static scope_t *s_scope_alt  = NULL;    /* 切组时先装到这里，成功再交换 */
static int      s_group      = -1;      /* 组序号，-1 = 没有清单，学全部字 */
static int      s_pos        = 0;       /* 当前字在组内的下标 */
static bool     s_sr_ready   = false;
static bool     s_dirty      = false;   /* 进度有变化，还没导出 */
static int64_t  s_dirty_at   = 0;
static int64_t  s_active_at  = 0;       /* 最近一次操作，用于自动关机 */
static scope_t *s_lookup     = NULL;    /* 查字表 = 所有清单合起来，开机读一次 */

static QueueHandle_t  s_cmd_q     = NULL;
static QueueHandle_t  s_disp_q    = NULL;
static volatile bool  s_disp_busy = false;
#if BOARD_HAS_EPAPER
static uint8_t       *s_epd_buf   = NULL;
#endif

static void post_cmd(int cmd)
{
    if (s_cmd_q && xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "命令队列满，丢弃 %d", cmd);
}

#if BOARD_HAS_TOUCH
/* 触摸补的是按键的缺口：这块板没有 GPIO 上的 PWR 键。
 * 轻点 = 单击 BOOT（唤醒语音），左右滑 = 翻字，和语音的「下一个/上一个」同一条命令。*/
static void on_touch(touch_evt_t evt)
{
    switch (evt) {
    case TOUCH_SWIPE_LEFT:  post_cmd(SR_CMD_NEXT); break;
    case TOUCH_SWIPE_RIGHT: post_cmd(SR_CMD_PREV); break;
    case TOUCH_TAP:
    default:                post_cmd(APP_EVT_WAKE_KEY); break;
    }
}
#endif

/* ======================= 刷屏 ======================= */

#if BOARD_HAS_EPAPER

static void put_px(uint8_t *buf, int x, int y, uint8_t c)
{
    if ((unsigned)x >= EPD_W || (unsigned)y >= EPD_H) return;
    uint8_t *b = &buf[y * (EPD_W / 4) + x / 4];
    int shift = 6 - 2 * (x % 4);                  /* 高位在前 */
    *b = (uint8_t)((*b & ~(3 << shift)) | (c << shift));
}

static void thick_line(uint8_t *buf, int x0, int y0, int x1, int y1, int r, uint8_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++)
                if (ox * ox + oy * oy <= r * r) put_px(buf, x + ox, y + oy, c);
    }
}

/* 已掌握的字在右上角画红勾（gen_images.py 在 x160-196 / y48-84 留了空）*/
static void draw_check(uint8_t *buf)
{
    thick_line(buf, 164, 66, 174, 78, 2, EPD_1IN54G_RED);
    thick_line(buf, 174, 78, 192, 52, 2, EPD_1IN54G_RED);
}

static bool load_image(int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/img/%04d.bin", SD_ROOT, id);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "读图失败: %s", path); return false; }
    size_t n = fread(s_epd_buf, 1, EPD_BUF_SIZE, f);
    fclose(f);
    if (n != EPD_BUF_SIZE) {
        ESP_LOGE(TAG, "图片尺寸异常 %s: %u 字节（应为 %d）", path, (unsigned)n, EPD_BUF_SIZE);
        return false;
    }
    return true;
}

/* 画面编码 = 字id*2 + 是否画红勾。存进 NVS，开机时对得上就不用重刷 */
static void display_task(void *arg)
{
    int code;
    for (;;) {
        if (xQueueReceive(s_disp_q, &code, portMAX_DELAY) != pdTRUE) continue;
        int id = code / 2;
        /* 插队换字会在队列里留下已经在屏上的字（刷 X 时换成 Y 又换回 X）。刷屏期间
         * shown 是 -1，request_display 那道「画面没变」挡不住，只能在这儿再挡一次 */
        if (code == app_store_get_shown()) {
            ESP_LOGI(TAG, "id=%d 已经在屏上，这一轮跳过", id);
        } else if (load_image(id)) {
            if (code & 1) draw_check(s_epd_buf);
            app_store_set_shown(-1);              /* 刷到一半断电 → 下次开机必刷 */
            ESP_LOGI(TAG, "刷屏 id=%d%s（约 20 秒）", id, (code & 1) ? " ✓" : "");
            /* 笔顺在这儿念，不在 show() 里念：插队排进来的字要等上一张刷完，
             * 在 show() 里念的话念完了字还没开始出现。（插队的字字音和词组早就播过了，
             * app_stroke_speak 仍按它俩还在前面算 head，摊出来的停顿偏小、比刷屏早念完
             * 几秒 —— 早一点正是想要的，不值得为它多开一个接口）*/
            if (s_cfg.stroke_order) app_stroke_speak(id, s_cfg.stroke_gap_ms);
            int64_t t0 = esp_timer_get_time();       /* 笔顺正是从这一刻开始念的 */
            epaper_port_display(s_epd_buf);
            app_store_set_shown(code);
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            ESP_LOGI(TAG, "刷屏完成 %.1f 秒", ms / 1000.0);
            app_stroke_set_window(ms);               /* 下一个字按这次的实测窗口摊停顿 */
        }
        if (uxQueueMessagesWaiting(s_disp_q) == 0) s_disp_busy = false;
    }
}

#elif BOARD_HAS_AMOLED   /* ---------- 368x448 RGB565 彩屏 ---------- */

/* SD 上的图就是整屏 RGB565 大端，直接 fread 进 PSRAM 再整块送屏，不做任何转换。
 * 目录是 img565/ 不是 img/ —— 一张卡要同时喂墨水屏板和这块板，两套图并存。
 * 生成脚本: content/gen_images_amoled.py */
static uint16_t *s_fb = NULL;

/* 已掌握的字在右上角画白勾（gen_images_amoled.py 的 MASTER_BOX 给它留了空）*/
#define CHECK_X  316
#define CHECK_Y  14

static void px565(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= AMOLED_W || y < 0 || y >= AMOLED_H) return;
    fb[(size_t)y * AMOLED_W + x] = c;
}

static void thick_line565(uint16_t *fb, int x0, int y0, int x1, int y1, int r, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    for (int i = 0; i <= steps; i++) {
        int x = x0 + dx * i / steps, y = y0 + dy * i / steps;
        for (int oy = -r; oy <= r; oy++)
            for (int ox = -r; ox <= r; ox++)
                if (ox * ox + oy * oy <= r * r) px565(fb, x + ox, y + oy, c);
    }
}

static void draw_check565(uint16_t *fb)
{
    /* 黑底上用亮绿，比红的显眼；40x40 的框里画一个对勾 */
    uint16_t g = amoled_rgb(90, 220, 110);
    thick_line565(fb, CHECK_X + 4,  CHECK_Y + 22, CHECK_X + 15, CHECK_Y + 33, 2, g);
    thick_line565(fb, CHECK_X + 15, CHECK_Y + 33, CHECK_X + 36, CHECK_Y + 6,  2, g);
}

static bool load_image(int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/img565/%04d.bin", SD_ROOT, id);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "读图失败: %s", path); return false; }
    size_t n = fread(s_fb, 1, AMOLED_FB_BYTES, f);
    fclose(f);
    if (n != AMOLED_FB_BYTES) {
        ESP_LOGE(TAG, "图片尺寸异常 %s: %u 字节（应为 %d）", path, (unsigned)n, AMOLED_FB_BYTES);
        return false;
    }
    return true;
}

/* ---------------- 米字格 ---------------- */
/* 小学练字本那种米字格。位置和 content/gen_anim.py 的 BOX_* 是同一个框，改一边就要改另一边。
 * 全量扫过 2500 张图，大字墨迹落在 y 115..320 / x 79..292，这个框四边都留了十几像素余量，
 * 又够不着拼音（底边 y=80）和分隔线（y=344）。*/
#define GRID_X  64
#define GRID_Y  98
#define GRID_S  240

/* 只落在黑底上 —— 这样静态图和动画能共用一次调用，格线永远压在字底下 */
static inline void grid_px(uint16_t *fb, int x, int y, uint16_t c)
{
    if (x < 0 || x >= AMOLED_W || y < 0 || y >= AMOLED_H) return;
    size_t i = (size_t)y * AMOLED_W + x;
    if (fb[i] == 0) fb[i] = c;
}

static void draw_grid565(uint16_t *fb)
{
    const uint16_t c = amoled_rgb(78, 26, 26);     /* 暗红：看得见，又不跟白字抢 */
    const int x1 = GRID_X + GRID_S - 1, y1 = GRID_Y + GRID_S - 1;

    for (int i = 0; i < GRID_S; i++) {            /* 外框实线 */
        grid_px(fb, GRID_X + i, GRID_Y, c); grid_px(fb, GRID_X + i, y1, c);
        grid_px(fb, GRID_X, GRID_Y + i, c); grid_px(fb, x1, GRID_Y + i, c);
    }
    /* 中线和对角线走虚线（练字本就是这么印的，也免得跟笔画混在一起）*/
    const int cx = GRID_X + GRID_S / 2, cy = GRID_Y + GRID_S / 2;
    for (int i = 0; i < GRID_S; i++) {
        if ((i / 6) & 1) continue;                 /* 实 6 虚 6 */
        grid_px(fb, GRID_X + i, cy, c);
        grid_px(fb, cx, GRID_Y + i, c);
        grid_px(fb, GRID_X + i, GRID_Y + i, c);
        grid_px(fb, GRID_X + i, y1 - i, c);
    }
}

/* ---------------- 笔顺动画 ---------------- */
/* 数据是 content/gen_anim.py 生成的 anim/<id>.bin：整个字的墨点按笔画分好组、
 * 组内按书写先后排好序，坐标是米字格框内的 (x,y)，一像素两字节。
 *
 * 关键：动画不自己画笔画，只是把静态图上那个字的像素按顺序放出来 ——
 * 所以写完留在屏上的就是静态图本身，一个像素都不差，不会写完之后字形跳一下。*/
#define ANIM_MAX_STROKES  32          /* 全量实测最多 23 画 */
#define ANIM_MAX_BYTES    (48 * 1024) /* 全量实测最大 34850 字节 */
#define ANIM_FRAME_MS     33          /* 约 30 fps；系统 tick 10 ms，实际落在 30/40 ms 一帧 */

/* 正在写的那一笔的颜色 */
#define HL_R 255
#define HL_G 190
#define HL_B 60

static uint8_t  *s_anim_raw = NULL;   /* anim/<id>.bin 原文 */
static uint16_t *s_glyph    = NULL;   /* 框内 240x240 的原始像素（从静态图上抠下来的）*/
static int       s_anim_drawn = 0;    /* 已经写到第几画；念两遍时用来判断要不要擦了重来 */
static struct {
    uint8_t        n;
    uint16_t       cnt[ANIM_MAX_STROKES];
    const uint8_t *px[ANIM_MAX_STROKES];
} s_anim;

static bool load_anim(int id, int want_strokes)
{
    s_anim.n = 0;
    if (!s_anim_raw || !s_glyph || want_strokes <= 0) return false;

    char path[64];
    snprintf(path, sizeof(path), "%s/anim/%04d.bin", SD_ROOT, id);
    FILE *f = fopen(path, "rb");
    if (!f) return false;                          /* 没有就退回静态显示，不算错 */
    size_t n = fread(s_anim_raw, 1, ANIM_MAX_BYTES, f);
    fclose(f);

    if (n < 12 || memcmp(s_anim_raw, "PSA1", 4) != 0) {
        ESP_LOGW(TAG, "笔顺动画文件不认识: %s", path); return false;
    }
    uint8_t ns = s_anim_raw[4], w = s_anim_raw[5], h = s_anim_raw[6];
    uint16_t x0, y0;
    memcpy(&x0, s_anim_raw + 8, 2);                /* 文件是小端，和 S3 一致 */
    memcpy(&y0, s_anim_raw + 10, 2);
    if (w != GRID_S || h != GRID_S || x0 != GRID_X || y0 != GRID_Y) {
        ESP_LOGW(TAG, "笔顺动画框对不上 %dx%d@%d,%d，固件要的是 %dx%d@%d,%d",
                 w, h, x0, y0, GRID_S, GRID_S, GRID_X, GRID_Y);
        return false;
    }
    if (ns == 0 || ns > ANIM_MAX_STROKES || ns != want_strokes) {
        ESP_LOGW(TAG, "笔顺动画 %d 画，笔顺表 %d 画，对不上", ns, want_strokes);
        return false;
    }

    size_t need = 12 + (size_t)ns * 2;
    if (n < need) { ESP_LOGW(TAG, "笔顺动画文件截断"); return false; }
    const uint8_t *p = s_anim_raw + need;
    size_t total = 0;
    for (int i = 0; i < ns; i++) {
        uint16_t c; memcpy(&c, s_anim_raw + 12 + i * 2, 2);
        s_anim.cnt[i] = c;
        s_anim.px[i]  = p + total * 2;
        total += c;
    }
    if (need + total * 2 != n) {
        ESP_LOGW(TAG, "笔顺动画像素数对不上: 头说 %u，文件 %u 字节",
                 (unsigned)(need + total * 2), (unsigned)n);
        return false;
    }
    s_anim.n = ns;
    return true;
}

static void save_glyph(const uint16_t *fb)
{
    for (int y = 0; y < GRID_S; y++)
        memcpy(&s_glyph[(size_t)y * GRID_S],
               &fb[(size_t)(GRID_Y + y) * AMOLED_W + GRID_X], GRID_S * 2);
}

static void blank_box(uint16_t *fb)
{
    for (int y = 0; y < GRID_S; y++)
        memset(&fb[(size_t)(GRID_Y + y) * AMOLED_W + GRID_X], 0, GRID_S * 2);
}

/* 原像素是白字，亮度藏在任一通道里。按它重新配一遍暖黄，抗锯齿的灰边跟着淡下去，
 * 边缘才不会毛 —— 一刀切填黄会把半透明的边描成硬边。*/
static inline uint16_t tint565(uint16_t be)
{
    uint32_t a = (uint32_t)(((be >> 8) | (be << 8)) & 0xFFFF) >> 11;   /* 红通道 0..31 */
    return amoled_rgb((uint8_t)(HL_R * a / 31), (uint8_t)(HL_G * a / 31), (uint8_t)(HL_B * a / 31));
}

/* 把第 idx 画的 [from,to) 个像素写进 s_fb，回填改动到的行范围（框内行号）*/
static void put_px(int idx, uint32_t from, uint32_t to, bool hl, int *ylo, int *yhi)
{
    const uint8_t *p = s_anim.px[idx];
    for (uint32_t i = from; i < to; i++) {
        int x = p[i * 2], y = p[i * 2 + 1];
        uint16_t src = s_glyph[(size_t)y * GRID_S + x];
        s_fb[(size_t)(GRID_Y + y) * AMOLED_W + GRID_X + x] = hl ? tint565(src) : src;
        if (y < *ylo) *ylo = y;
        if (y > *yhi) *yhi = y;
    }
}

/* 逐画回调 —— app_stroke 在这一画的名字开始响的那一刻调进来，
 * 我们就在念这个名字的 dur_ms 里把这一笔写出来。*/
static bool anim_step(int idx, int n, int dur_ms, void *ctx)
{
    (void)n;
    if (idx < 0 || idx >= s_anim.n) return true;

    if (idx == 0 && s_anim_drawn > 0) {           /* 短字念两遍：第二遍擦了重写 */
        blank_box(s_fb);
        draw_grid565(s_fb);
        amoled_port_draw_rows(s_fb, GRID_Y, GRID_S);
        s_anim_drawn = 0;
    }

    uint32_t cnt = s_anim.cnt[idx];
    int frames = dur_ms / ANIM_FRAME_MS;
    if (frames < 2)  frames = 2;
    if (frames > 40) frames = 40;

    int64_t t0 = esp_timer_get_time();
    uint32_t done = 0;
    for (int f = 1; f <= frames; f++) {
        uint32_t upto = (uint32_t)((uint64_t)cnt * f / frames);
        int ylo = GRID_S, yhi = -1;
        put_px(idx, done, upto, true, &ylo, &yhi);
        done = upto;
        /* 只送真正改了的那几行：整屏 29 ms，几行 1~2 ms */
        if (yhi >= 0) amoled_port_draw_rows(s_fb, GRID_Y + ylo, yhi - ylo + 1);

        /* 来新字了就立刻收手，别让孩子等着看完错字的笔顺 */
        if (uxQueueMessagesWaiting(s_disp_q) > 0) { *(bool *)ctx = false; return false; }

        int wait = (int)((t0 + (int64_t)dur_ms * 1000 * f / frames - esp_timer_get_time()) / 1000);
        if (wait > 0) vTaskDelay(pdMS_TO_TICKS(wait));
    }

    int ylo = GRID_S, yhi = -1;                   /* 这一画写完，从高亮转成正式的白 */
    put_px(idx, 0, cnt, false, &ylo, &yhi);
    if (yhi >= 0) amoled_port_draw_rows(s_fb, GRID_Y + ylo, yhi - ylo + 1);
    s_anim_drawn = idx + 1;
    return true;
}

/* 彩屏刷屏只要一百多毫秒，墨水屏那套「把笔顺停顿摊进 20 秒刷屏窗口」在这儿没有意义。
 * 这块板换了个玩法：字不直接给，先只摆米字格，然后跟着笔画名一笔一笔写出来 ——
 * 念「横」的时候横正在长，念完停 600 ms 让孩子跟着写一笔。*/
#define STROKE_GAP_FAST_MS  600

static void display_task(void *arg)
{
    int code;
    for (;;) {
        if (xQueueReceive(s_disp_q, &code, portMAX_DELAY) != pdTRUE) continue;
        int id = code / 2;
        if (code == app_store_get_shown()) {
            ESP_LOGI(TAG, "id=%d 画面没变，跳过", id);
        } else if (load_image(id)) {
            if (code & 1) draw_check565(s_fb);
            app_store_set_shown(-1);              /* 刷到一半断电 → 下次开机必刷 */

            /* 要写笔顺就先把字收起来，屏上只留拼音、词组和空米字格 */
            bool anim = s_cfg.stroke_order && load_anim(id, app_stroke_count(id));
            if (anim) { save_glyph(s_fb); blank_box(s_fb); }
            draw_grid565(s_fb);
            s_anim_drawn = 0;

            int64_t t0 = esp_timer_get_time();
            amoled_port_draw(s_fb);
            ESP_LOGI(TAG, "刷屏 id=%d%s %d ms%s", id, (code & 1) ? " ✓" : "",
                     (int)((esp_timer_get_time() - t0) / 1000),
                     anim ? " → 写笔顺" : "");

            bool ok = true;
            if (s_cfg.stroke_order)
                app_stroke_speak_ex(id, s_cfg.stroke_gap_ms > 0 ? s_cfg.stroke_gap_ms
                                                                : STROKE_GAP_FAST_MS,
                                    anim ? anim_step : NULL, &ok);
            /* 被插队打断时字只写了一半，别记成「屏上已经是这个字了」，
             * 不然下次再查到它会判定画面没变、直接跳过 */
            if (ok) app_store_set_shown(code);
        }
        if (uxQueueMessagesWaiting(s_disp_q) == 0) s_disp_busy = false;
    }
}

#else  /* 两块屏都没有 —— P0 阶段的无屏版本 */

/* 没有屏，但这个任务不能省：笔顺朗读是在 display_task 里发起的，不在 show() 里。
 * 直接砍掉这个任务，笔顺就一声不响地没了 —— 而笔顺恰恰是 P0 要验证的东西之一。
 * 所以保留整条链路（队列 → 任务 → app_stroke_speak），只把读图和刷面板摘掉。
 *
 * 少了那 20 秒窗口，停顿也没法再「摊」了：自动模式会按 20.5 秒的初值算，
 * 每画之间能拉出一两秒的空。没屏的时候按固定节奏念就行。*/
#define STROKE_GAP_NOSCREEN_MS  600

static void display_task(void *arg)
{
    int code;
    for (;;) {
        if (xQueueReceive(s_disp_q, &code, portMAX_DELAY) != pdTRUE) continue;
        int id = code / 2;
        if (code == app_store_get_shown()) {
            ESP_LOGI(TAG, "id=%d 画面没变，跳过", id);
        } else {
            ESP_LOGI(TAG, "（无屏）id=%d%s", id, (code & 1) ? " ✓" : "");
            if (s_cfg.stroke_order)
                app_stroke_speak(id, s_cfg.stroke_gap_ms > 0 ? s_cfg.stroke_gap_ms
                                                             : STROKE_GAP_NOSCREEN_MS);
            app_store_set_shown(code);
        }
        if (uxQueueMessagesWaiting(s_disp_q) == 0) s_disp_busy = false;
    }
}

#endif /* 屏 */

/* 排一次刷屏。画面本来就对（开机恢复上次的字、重复按同一个字）就不刷 ——
 * 不刷就没有那 17.6 秒的空档，display_task 也就不会念笔顺 */
static void request_display(int id)
{
    int code = id * 2 + (app_store_is_mastered(id) ? 1 : 0);
    if (code == app_store_get_shown()) { ESP_LOGI(TAG, "画面没变，不刷屏"); return; }
    if (s_disp_busy) {
        /* 查错了字不用干等着刷完: 队列深度 1、后者覆盖前者，这一张刷完直接刷最新的。
         * 但面板那 17.6 秒的波形掐不断（见 epaper_port_display），插队省掉的是错字的
         * 笔顺朗读和再喊一遍，不是那 17.6 秒。字音和词组已经排在前面了，孩子知道听见了，
         * 再补一句「等一下哦」，免得后面这十几秒静音显得像死机 */
        ESP_LOGI(TAG, "正在刷屏，id=%d 排队，刷完接着上", id);
#if BOARD_HAS_EPAPER
        /* 彩屏刷一张一百多毫秒，s_disp_busy 几乎不可能被撞上；真撞上了也没有十几秒
         * 的静音要解释，一句「等一下哦」反而多余。这句提示只给墨水屏 */
        app_audio_prompt("busy", TONE_BUSY);
#endif
    }
    s_disp_busy = true;
    xQueueOverwrite(s_disp_q, &code);
}

/* ======================= 学习逻辑（cmd_task）======================= */

static int cur_id(void) { return s_scope->n ? s_scope->ids[s_pos] : -1; }
static const char *cur_char(void) { return s_scope->n ? s_scope->chars[s_pos] : ""; }

static void export_progress(void)
{
    app_store_export_progress(PROGRESS_PATH, SCOPE_DIR, s_total, s_scope->name, cur_char());
    s_dirty = false;
}

static void mark_dirty(void)
{
    s_dirty = true;
    s_dirty_at = esp_timer_get_time();
}

/* 呈现当前字：记进度 → 读字音和词组 → 刷屏（三者并行）。
 * 刷屏要 17.6 秒，字音 + 词组只占 4 秒左右，剩下的空档用笔顺填上：
 * 孩子跟着读音和笔顺在纸上写，写完抬头，字刚好出现在屏幕上 */
static void show(void)
{
    int id = cur_id();
    if (id < 0) return;
    ESP_LOGI(TAG, "→ %s 第 %d/%d 个「%s」 id=%d%s", s_scope->name[0] ? s_scope->name : "全部字",
             s_pos + 1, s_scope->n, cur_char(), id, app_store_is_mastered(id) ? " (已会)" : "");
    app_store_save_pos(s_scope->name, id);
    app_audio_play_char(id, 'c');
    app_audio_play_char(id, 'w');
    request_display(id);                          /* 笔顺由 display_task 在刷屏开始时念 */
}

static void goto_pos(int pos)
{
    int n = s_scope->n;
    if (n <= 0) return;
    s_pos = ((pos % n) + n) % n;                  /* 组内循环 */
    app_audio_stop();
    show();
}

/* 从 start 开始（含）往后找第一个没掌握的字，循环一圈；都会了返回 -1 */
static int find_unmastered(int start)
{
    int n = s_scope->n;
    for (int k = 0; k < n; k++) {
        int i = (start + k) % n;
        if (!app_store_is_mastered(s_scope->ids[i])) return i;
    }
    return -1;
}

/* 把第 g 组装进 s_scope（先装到备用的，装成功再交换）*/
static bool load_group(int g)
{
    if (!scope_load(s_scope_alt, SCOPE_DIR, app_groups_name(g), s_total)) return false;
    scope_t *t = s_scope; s_scope = s_scope_alt; s_scope_alt = t;
    s_group = g;
    return true;
}

static int pos_of(int id)
{
    for (int i = 0; i < s_scope->n; i++)
        if (s_scope->ids[i] == id) return i;
    return -1;
}

/* 下一个 / 上一个：到组尾接着下一组的第一个字，在组头往前是上一组的最后一个字。
 * 语音不能切组（查字不分组），靠这个把所有组串起来 */
static void step(int d)
{
    int pos = s_pos + d, gc = app_groups_count();
    if ((pos < 0 || pos >= s_scope->n) && s_group >= 0 && gc > 1) {
        int g = ((s_group + d) % gc + gc) % gc;
        if (load_group(g)) {
            pos = d > 0 ? 0 : s_scope->n - 1;
            ESP_LOGI(TAG, "%s第 %d 组 %s（%d 字）", d > 0 ? "接着学" : "回到", g + 1, s_scope->name, s_scope->n);
        }
    }
    goto_pos(pos);
}

/* 查字：本组有就在组内跳，没有就换到它所在的组 */
static void jump_to_char(int id)
{
    int pos = pos_of(id);
    if (pos < 0) {
        int g = app_groups_of(id);
        if (g < 0 || !load_group(g)) {
            ESP_LOGW(TAG, "清单里没有字 id=%d", id);
            app_audio_stop();
            app_audio_prompt("not_found", TONE_TIMEOUT);
            return;
        }
        pos = pos_of(id);
        if (pos < 0) pos = 0;                          /* 开机后清单文件被改过 */
        ESP_LOGI(TAG, "查字 → 第 %d 组 %s", g + 1, s_scope->name);
    }
    goto_pos(pos);
}

/* 切组，只有控制台能切（调试用）*/
static void switch_group(int g)
{
    int gc = app_groups_count();
    if (gc == 0) { ESP_LOGW(TAG, "SD 卡上没有学习清单"); app_audio_tone(TONE_TIMEOUT); return; }
    g = ((g % gc) + gc) % gc;
    if (!load_group(g)) { app_audio_tone(TONE_TIMEOUT); return; }
    int first = find_unmastered(0);
    s_pos = first < 0 ? 0 : first;
    ESP_LOGI(TAG, "切换到第 %d 组 %s（%d 字）", g + 1, s_scope->name, s_scope->n);

    app_audio_stop();
    char key[24];
    snprintf(key, sizeof(key), "group_%02d", g + 1);
    app_audio_prompt(key, TONE_GROUP);
    if (first < 0) app_audio_prompt("all_done", TONE_DONE);
    mark_dirty();
    show();
}

static void log_status(void)
{
    int k = 0;
    for (int i = 0; i < s_scope->n; i++) k += app_store_is_mastered(s_scope->ids[i]);
    ESP_LOGI(TAG, "状态: 组 %d/%d %s  第 %d/%d 个「%s」 id=%d",
             s_group + 1, app_groups_count(), s_scope->name, s_pos + 1, s_scope->n, cur_char(), cur_id());
    ESP_LOGI(TAG, "      本组已会 %d/%d  总计已会 %d/%d  刷屏中=%d  播放中=%d  语音=%s",
             k, s_scope->n, app_store_mastered_count(), s_total,
             s_disp_busy, !app_audio_idle(), s_sr_ready ? "就绪" : "未就绪");
    ESP_LOGI(TAG, "      电池 %d mV  USB=%d  内部 RAM 剩余 %u  PSRAM 剩余 %u",
             app_power_vbat_mv(), app_power_usb_connected(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* 列出本组的字、字 id 和查字说法（控制台 c 汉字 / c 字id 查字）*/
static void log_scope(void)
{
    if (!s_scope->name[0]) { ESP_LOGI(TAG, "没有学习清单（全部字模式），不能语音查字"); return; }
    ESP_LOGI(TAG, "第 %d 组 %s 共 %d 字（查字表共 %d 字），字 id 和查字说法:",
             s_group + 1, s_scope->name, s_scope->n, s_lookup ? s_lookup->n : 0);
    for (int i = 0; i < s_scope->n; i++)
        ESP_LOGI(TAG, "  %-4d %s%s  %s", s_scope->ids[i], s_scope->chars[i],
                 app_store_is_mastered(s_scope->ids[i]) ? "✓" : " ",
                 s_scope->phrases[i][0] ? s_scope->phrases[i] : "(无)");
}

static void power_off(const char *why)
{
    ESP_LOGI(TAG, "关机（%s）", why);
    app_audio_stop();
    app_audio_prompt("bye", TONE_BYE);
    for (int i = 0; i < 50 && !app_audio_idle(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    /* 刷屏中途断电会让墨水屏停在半刷状态，等它刷完。插队换字时队列里可能还压着一张，
     * s_disp_busy 要跨两次刷屏（17.6×2 = 35.2 秒）才落下，上限给到 40 秒 */
    for (int i = 0; i < 400 && s_disp_busy; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_dirty) export_progress();
#if BOARD_HAS_EPAPER
    epaper_port_sleep();
#endif
    app_power_off();
}

/* 刷屏期间命令照常执行，不再挡着: 换字排进显示队列（深度 1，后者覆盖前者），
 * 这一张刷完直接刷最新的那个 —— 查错了字不用干等着它刷完。错字的笔顺朗读由
 * goto_pos 里那句 app_audio_stop() 当场掐掉 */
static void handle_learn_cmd(int cmd)
{
    int id = cur_id();
    if (id < 0) return;

    if (cmd >= APP_CMD_GROUP_BASE) {                   /* 控制台 g N，N 从 1 开始 */
        int g = cmd - APP_CMD_GROUP_BASE;
        if (g < 1 || g > app_groups_count()) { ESP_LOGW(TAG, "没有第 %d 组", g); return; }
        switch_group(g - 1);
        return;
    }
    if (cmd >= SR_CMD_CHAR_BASE) {                     /* 「如果的如」→ 如的字 id */
        jump_to_char(cmd - SR_CMD_CHAR_BASE);
        return;
    }

    switch (cmd) {
    case SR_CMD_NEXT:   step(1); break;
    case SR_CMD_PREV:   step(-1); break;
    case SR_CMD_REPEAT: app_audio_stop(); app_audio_play_char(id, 'c'); break;
    case SR_CMD_WORDS:  app_audio_stop(); app_audio_play_char(id, 'w'); break;

    case SR_CMD_MASTERED: {
        if (app_store_set_mastered(id, true)) mark_dirty();
        ESP_LOGI(TAG, "我会了「%s」 已会 %d 字", cur_char(), app_store_mastered_count());
        int nx = find_unmastered(s_pos + 1);
        app_audio_stop();
        app_audio_prompt("mastered", TONE_OK);
        if (nx < 0) {
            ESP_LOGI(TAG, "本组全部会了");
            app_audio_prompt("all_done", TONE_DONE);
            nx = (s_pos + 1) % s_scope->n;
        }
        s_pos = nx;
        show();
        break;
    }
    case SR_CMD_REVIEW: {                              /* 下一个没掌握的字，当前字排最后 */
        int nx = find_unmastered(s_pos + 1);
        if (nx < 0) {
            ESP_LOGI(TAG, "复习：本组全部会了");
            app_audio_stop();
            app_audio_prompt("all_done", TONE_DONE);
        } else {
            ESP_LOGI(TAG, "复习 → 第 %d 个字", nx + 1);
            goto_pos(nx);
        }
        break;
    }
    case SR_CMD_FORGOT: {
        bool changed = app_store_set_mastered(id, false);
        ESP_LOGI(TAG, "忘了「%s」%s", cur_char(), changed ? "，取消标记" : "（本来就没标记）");
        app_audio_stop();
        app_audio_prompt("forgot", TONE_FORGOT);
        app_audio_play_char(id, 'c');
        app_audio_play_char(id, 'w');
        /* 去掉红勾要刷屏，同样是 17.6 秒的空档。孩子说「忘了」正是最该跟着笔顺
         * 重写一遍的时候，所以这里也念（display_task 里刷屏一开始就念）。
         * head 里还多一句 forgot 提示音没算进去，摊出来的停顿会偏大一点点 */
        if (changed) {
            mark_dirty();
            request_display(id);
        }
        break;
    }
    case APP_CMD_GROUP_NEXT: switch_group(s_group + 1); break;
    case APP_CMD_GROUP_PREV: switch_group(s_group - 1); break;
    default: break;
    }
}

static void handle_cmd(int cmd)
{
    switch (cmd) {
    case EVT_SR_READY:
        s_sr_ready = true;
        if (s_lookup) app_sr_set_lookup(s_lookup->ids, s_lookup->phrases, s_lookup->n);
        else ESP_LOGW(TAG, "没有学习清单，只有固定命令，不能查字");
        ESP_LOGI(TAG, "语音就绪 —— 说「你好小智」唤醒");
        return;
    case SR_EVT_NOT_FOUND:  app_audio_prompt("not_found", TONE_TIMEOUT); break;
    case APP_EVT_STATUS:    log_status(); return;
    case APP_EVT_LIST:      log_scope(); return;
    case APP_EVT_EXPORT:    export_progress(); return;
    case APP_EVT_POWER_OFF: power_off("长按 PWR"); return;
    case APP_EVT_WAKE_KEY:
        /* 不用喊「你好小智」，按一下就能说话。按这个键就表示「我要说话了」，
         * 所以先把正在念的笔顺掐掉 —— 不然孩子得等十几秒才轮到自己开口，
         * 而且播放期间麦克风是屏蔽的（见 app_sr 的静音帧不计时），压根听不见。
         * 提示音不在这儿放: app_sr 进了监听窗口才回调 SR_EVT_WAKE，由下面那行放 */
        if (!s_sr_ready) { ESP_LOGW(TAG, "语音还没就绪，按键唤醒无效"); return; }
        ESP_LOGI(TAG, "按键唤醒 → 等孩子说话");
        app_audio_stop();
        app_sr_wake();
        break;
    case SR_EVT_WAKE:       app_audio_prompt("wake", TONE_WAKE); break;
    case SR_EVT_TIMEOUT:    app_audio_prompt("timeout", TONE_TIMEOUT); break;
    default:                if (cmd >= 0) handle_learn_cmd(cmd); break;
    }
    s_active_at = esp_timer_get_time();
}

static void command_task(void *arg)
{
    s_active_at = esp_timer_get_time();
    for (;;) {
        int cmd;
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) handle_cmd(cmd);

        int64_t now = esp_timer_get_time();
        bool idle = !s_disp_busy && app_audio_idle();
        if (s_dirty && idle && now - s_dirty_at > EXPORT_DELAY_US) export_progress();

        if (s_cfg.sleep_minutes > 0 && idle &&
            now - s_active_at > (int64_t)s_cfg.sleep_minutes * 60 * 1000 * 1000) {
            if (app_power_usb_connected()) s_active_at = now;   /* 连着电脑调试时不自动关机 */
            else power_off("长时间无操作");
        }
    }
}

/* ======================= 按键 ======================= */
/* BOOT: 单击 唤醒（等孩子说话）  双击 下一个   长按 读词组
 * PWR : 单击 我会了              双击 复习     长按 关机
 * 单击 BOOT 是最常用的那一下，给了唤醒: 喊唤醒词不一定能唤上（周围吵、声音小），
 * 按键百分之百唤得上。代价是「重播字音」没有按键了，只剩说「再读一遍」和控制台 r */
static void button_task(void *arg)
{
    /* 电池开机就是按住 PWR：松手 0.5 秒前的 PWR 事件都不算，否则一开机就被当成长按关机 */
    int64_t pwr_ignore_until = app_power_key_down() ? INT64_MAX : 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        int64_t now = esp_timer_get_time();
        EventBits_t b = xEventGroupClearBits(boot_groups, 0x0F);   /* 返回清除前的值 */
        EventBits_t p = xEventGroupClearBits(pwr_groups, 0x0F);
        if (pwr_ignore_until == INT64_MAX && !app_power_key_down()) pwr_ignore_until = now + 500 * 1000;
        if (now < pwr_ignore_until) p = 0;

        /* 双击 / 长按时单击位也可能同时置起，按优先级判断 */
        if      (get_bit_button(b, BOOT_LONG))   { ESP_LOGI(TAG, "长按 BOOT → 读词组"); post_cmd(SR_CMD_WORDS); }
        else if (get_bit_button(b, BOOT_DOUBLE)) { ESP_LOGI(TAG, "双击 BOOT → 下一个"); post_cmd(SR_CMD_NEXT); }
        else if (get_bit_button(b, BOOT_SINGLE)) { ESP_LOGI(TAG, "单击 BOOT → 唤醒");   post_cmd(APP_EVT_WAKE_KEY); }

        if      (get_bit_button(p, PWR_LONG))    { ESP_LOGI(TAG, "长按 PWR → 关机");    post_cmd(APP_EVT_POWER_OFF); }
        else if (get_bit_button(p, PWR_DOUBLE))  { ESP_LOGI(TAG, "双击 PWR → 复习");    post_cmd(SR_CMD_REVIEW); }
        else if (get_bit_button(p, PWR_SINGLE))  { ESP_LOGI(TAG, "单击 PWR → 我会了");  post_cmd(SR_CMD_MASTERED); }
    }
}

/* ======================= 初始化 ======================= */

static int load_total(void)
{
    FILE *f = fopen(SD_ROOT "/index.txt", "r");
    int n = 0;
    if (!f || fscanf(f, "%d", &n) != 1) n = 0;
    if (f) fclose(f);
    if (n <= 0 || n > STORE_MAX_IDS) { ESP_LOGE(TAG, "读不到 %s/index.txt", SD_ROOT); return 0; }
    ESP_LOGI(TAG, "生字总数: %d", n);
    return n;
}

/* 恢复上次的组和字；组文件没了就回到第 1 组，一个清单都没有就学全部字 */
static void restore_position(void)
{
    char name[SCOPE_NAME_LEN];
    int id;
    app_store_get_pos(name, sizeof(name), &id);
    int g = name[0] ? app_groups_find(name) : -1;
    if (g < 0 && name[0]) ESP_LOGW(TAG, "上次的清单 %s 不在了，从第 1 组开始", name);
    if (g < 0 && app_groups_count() > 0) g = 0;

    if (g >= 0 && scope_load(s_scope, SCOPE_DIR, app_groups_name(g), s_total)) {
        s_group = g;
    } else {
        ESP_LOGW(TAG, "没有可用的学习清单，按字表顺序学全部 %d 字", s_total);
        scope_fill_all(s_scope, s_total);
        s_group = -1;
    }
    s_pos = 0;
    for (int i = 0; i < s_scope->n; i++)
        if (s_scope->ids[i] == id) { s_pos = i; break; }
}


void user_app_init(void)
{
    ESP_LOGI(TAG, "===== 小学生字学习机启动 =====");

#if BOARD_HAS_AMOLED
    /* I2C 得在屏之前起来 —— 屏的供电和复位挂在 TCAL9534(0x20) 上，走的就是这条总线。
     * I2C 在 15/14，和屏的 QSPI（4/5/6/7/11/12）不冲突。*/
    i2c_master_Init();
    i2c_bus_scan();

    if (amoled_port_init()) {
        amoled_port_brightness(AMOLED_BRIGHTNESS);
#if AMOLED_TEST_PATTERN
        amoled_port_test_pattern();
#endif
    }

#endif

#if BOARD_HAS_TOUCH
    /* 必须排在 amoled_port_init() 之后 —— 触摸的复位脚和屏一样挂在 TCAL9534 上 */
    touch_bsp_init(on_touch);
#endif

    app_power_init();                    /* 电池自锁 + 屏幕/音频电源域 */
    app_store_init();                    /* NVS 进度 */

#if BOARD_HAS_AMOLED
    /* 彩屏断电就没画面了，NVS 里记的「屏上是哪一张」只对墨水屏成立（墨水屏断电还留着字）。
     * 不清掉的话开机恢复上次的字时 request_display 会判定「画面没变」，屏就一直黑着。
     * 必须排在 app_store_init() 之后 —— 它会从 NVS 把 s_shown 读回来。*/
    app_store_set_shown(-1);
#endif

#if !BOARD_HAS_AMOLED
    i2c_master_Init();
#endif
    user_button_init();
    audio_bsp_init();
    audio_play_init();                   /* 16kHz / 2ch / 16bit，播放+录音同时打开 */

    /* 注意 _sdcard_init() 成功返回 1、失败返回 0，与常见约定相反 */
    if (_sdcard_init() != 1) ESP_LOGE(TAG, "SD 卡初始化失败 —— 请确认已插卡且为 FAT32");

    app_store_load_config(CONFIG_PATH, &s_cfg);
    app_audio_init(SD_ROOT);
    app_audio_set_volume(s_cfg.volume);

#if BOARD_HAS_EPAPER
    s_epd_buf = (uint8_t *)heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    assert(s_epd_buf);
    epaper_port_init();
#elif BOARD_HAS_AMOLED
    /* 整屏 322 KB 只能放 PSRAM。面板是从这块缓冲直接 DMA 出去的（分条送，见 panel_push），
     * 所以送屏期间不能改它 —— display_task 是唯一的写入者，天然满足。*/
    s_fb = (uint16_t *)heap_caps_malloc(AMOLED_FB_BYTES, MALLOC_CAP_SPIRAM);
    assert(s_fb);
    /* 笔顺动画的两块: 米字格框内的原始像素 + anim/<id>.bin 原文。
     * 分不到也不致命，load_anim 会退回静态显示。*/
    s_glyph    = (uint16_t *)heap_caps_malloc(GRID_S * GRID_S * 2, MALLOC_CAP_SPIRAM);
    s_anim_raw = (uint8_t  *)heap_caps_malloc(ANIM_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_glyph || !s_anim_raw) ESP_LOGW(TAG, "笔顺动画内存不够，只做静态显示");
#endif

    s_total = load_total();
    if (s_total <= 0) {
        ESP_LOGE(TAG, "没有可用生字数据");
        app_audio_tone(TONE_TIMEOUT);
#if BOARD_HAS_EPAPER
        epaper_port_clear(EPD_1IN54G_WHITE);
#endif
        app_store_set_shown(-1);
        if (!app_power_usb_connected()) {        /* 电池供电就别空耗电 */
            vTaskDelay(pdMS_TO_TICKS(2000));
#if BOARD_HAS_EPAPER
            epaper_port_sleep();
#endif
            app_power_off();
        }
        return;
    }

    s_scope     = scope_new(s_total);
    s_scope_alt = scope_new(s_total);
    assert(s_scope && s_scope_alt);
    app_groups_scan(SCOPE_DIR);
    s_lookup = app_groups_load_all(s_total);   /* 查字表: 所有清单合起来 */
    app_stroke_init(SD_ROOT, s_total);         /* 笔顺朗读；老卡上没有 stroke/ 就自动关掉 */
    restore_position();

    s_disp_q = xQueueCreate(1, sizeof(int));
    s_cmd_q  = xQueueCreate(8, sizeof(int));
    assert(s_disp_q && s_cmd_q);
    xTaskCreatePinnedToCore(display_task, "disp_task", 4 * 1024, NULL, 3, NULL, 1);

    show();                              /* 墨水屏断电保留画面：和上次一样就不刷 */

    xTaskCreatePinnedToCore(command_task, "cmd_task",    6 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(button_task,  "button_task", 3 * 1024, NULL, 4, NULL, 1);
    app_console_start(post_cmd);
    ESP_LOGI(TAG, "按键: BOOT 单击=下一个 双击=重播 长按=词组 | PWR 单击=我会了 双击=复习 长按=关机");

    /* 语音模型加载要几秒，和第一次刷屏并行；就绪后交给 cmd_task 注册命令词 */
    sr_config_t sr = {};
    sr.wake_high    = s_cfg.wake_high;
    sr.mn_threshold = s_cfg.mn_threshold;
    sr.listen_ms    = s_cfg.listen_seconds * 1000;
    sr.follow_up_ms = s_cfg.follow_up_seconds * 1000;
    if (app_sr_start(post_cmd, &sr)) post_cmd(EVT_SR_READY);
    else ESP_LOGW(TAG, "语音识别未启动，按键仍可用");
}
