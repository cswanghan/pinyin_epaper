/*****************************************************************************
 * 小学生字学习机 —— 主应用
 *
 * 硬件: Waveshare ESP32-S3-ePaper-1.54G (200x200 四色墨水屏 + ES8311 音频 + SD)
 *
 * 任务划分:
 *   cmd_task      唯一持有学习状态（当前组 / 当前字 / 掌握标记）。语音、按键、
 *                 控制台都只往 s_cmd_q 投递命令，由它串行处理 —— 不用加锁
 *   display_task  刷屏（阻塞约 20 秒）。刷屏期间只接受"重播字音 / 读词组"，
 *                 其余命令回一句「等一下哦」，避免排队的命令在 20 秒后突然生效
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
#include "epaper_port.h"
#include "app_sr.h"
#include "app_audio.h"
#include "app_store.h"
#include "app_power.h"
#include "app_console.h"

#define TAG "PINYIN"

#define EPD_W          200
#define EPD_H          200
#define EPD_BUF_SIZE   ((EPD_W / 4) * EPD_H)      /* 4 像素/字节 = 10000 */

#define SD_ROOT        "/sdcard/pinyin"
#define SCOPE_DIR      SD_ROOT "/scope"
#define CONFIG_PATH    SD_ROOT "/config.txt"
#define PROGRESS_PATH  SD_ROOT "/progress.txt"

#define EXPORT_DELAY_US     (30LL * 1000 * 1000)  /* 进度变化后空闲 30 秒再导出 */
#define BUSY_PROMPT_GAP_US  (2LL * 1000 * 1000)   /* 「等一下哦」不要连着说 */

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
static int64_t  s_busy_said  = 0;
static int64_t  s_show_at    = 0;       /* 换组后推迟到这个时刻再呈现，0 = 没有 */

static QueueHandle_t  s_cmd_q     = NULL;
static QueueHandle_t  s_disp_q    = NULL;
static volatile bool  s_disp_busy = false;
static uint8_t       *s_epd_buf   = NULL;

static void post_cmd(int cmd)
{
    if (s_cmd_q && xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE)
        ESP_LOGW(TAG, "命令队列满，丢弃 %d", cmd);
}

/* ======================= 刷屏 ======================= */

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
        if (load_image(id)) {
            if (code & 1) draw_check(s_epd_buf);
            app_store_set_shown(-1);              /* 刷到一半断电 → 下次开机必刷 */
            ESP_LOGI(TAG, "刷屏 id=%d%s（约 20 秒）", id, (code & 1) ? " ✓" : "");
            int64_t t0 = esp_timer_get_time();
            epaper_port_display(s_epd_buf);
            app_store_set_shown(code);
            ESP_LOGI(TAG, "刷屏完成 %.1f 秒", (esp_timer_get_time() - t0) / 1e6);
        }
        if (uxQueueMessagesWaiting(s_disp_q) == 0) s_disp_busy = false;
    }
}

static void request_display(int id)
{
    int code = id * 2 + (app_store_is_mastered(id) ? 1 : 0);
    if (code == app_store_get_shown()) { ESP_LOGI(TAG, "画面没变，不刷屏"); return; }
    s_disp_busy = true;                           /* 先置忙，再投递，不给命令插空子 */
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

/* 呈现当前字：记进度 → 读字音和词组 → 刷屏（三者并行）*/
static void show(void)
{
    s_show_at = 0;
    int id = cur_id();
    if (id < 0) return;
    ESP_LOGI(TAG, "→ %s 第 %d/%d 个「%s」 id=%d%s", s_scope->name[0] ? s_scope->name : "全部字",
             s_pos + 1, s_scope->n, cur_char(), id, app_store_is_mastered(id) ? " (已会)" : "");
    app_store_save_pos(s_scope->name, id);
    app_audio_play_char(id, 'c');
    app_audio_play_char(id, 'w');
    request_display(id);
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

static void sr_update_scope(void)
{
    if (s_sr_ready)
        app_sr_set_scope(s_scope->phrases, s_scope->n, app_groups_count());
}

/* 换组后推迟呈现的时长：连续对话窗口 + 1 秒（窗口末尾说的话还要识别完）*/
static int64_t defer_us(void) { return (int64_t)(s_cfg.follow_up_seconds + 1) * 1000 * 1000; }

static void switch_group(int g)
{
    int gc = app_groups_count();
    if (gc == 0) { ESP_LOGW(TAG, "SD 卡上没有学习清单"); app_audio_tone(TONE_TIMEOUT); return; }
    g = ((g % gc) + gc) % gc;
    if (!scope_load(s_scope_alt, SCOPE_DIR, app_groups_name(g), s_total)) {
        app_audio_tone(TONE_TIMEOUT);
        return;
    }
    scope_t *t = s_scope; s_scope = s_scope_alt; s_scope_alt = t;
    s_group = g;
    int first = find_unmastered(0);
    s_pos = first < 0 ? 0 : first;
    ESP_LOGI(TAG, "切换到第 %d 组 %s（%d 字）", g + 1, s_scope->name, s_scope->n);

    app_audio_stop();
    char key[24];
    snprintf(key, sizeof(key), "group_%02d", g + 1);
    app_audio_prompt(key, TONE_GROUP);
    if (first < 0) app_audio_prompt("all_done", TONE_DONE);
    sr_update_scope();
    mark_dirty();

    /* 先不呈现：换组后常常紧接着说要学的字（「如果的如」）。等一个连续对话窗口，
     * 说了就直接刷到那个字，没说再呈现本组第一个字 —— 刷一次屏 21 秒，别白刷 */
    if (s_sr_ready && s_cfg.follow_up_seconds > 0) {
        s_show_at = esp_timer_get_time() + defer_us();
        ESP_LOGI(TAG, "先不刷屏，提示音放完再等 %d 秒，看要不要跳字", s_cfg.follow_up_seconds + 1);
    } else {
        show();
    }
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

/* 列出本组的字和跳字说法 —— 跳字命令只注册了本组，别的组的字说了不会有反应 */
static void log_scope(void)
{
    if (!s_scope->name[0]) { ESP_LOGI(TAG, "没有学习清单（全部字模式），没有跳字命令"); return; }
    ESP_LOGI(TAG, "第 %d 组 %s 共 %d 字，跳字说法:", s_group + 1, s_scope->name, s_scope->n);
    for (int i = 0; i < s_scope->n; i++)
        ESP_LOGI(TAG, "  c %-3d %s%s  %s", i, s_scope->chars[i],
                 app_store_is_mastered(s_scope->ids[i]) ? "✓" : " ",
                 s_scope->phrases[i][0] ? s_scope->phrases[i] : "(无)");
}

static void power_off(const char *why)
{
    ESP_LOGI(TAG, "关机（%s）", why);
    app_audio_stop();
    app_audio_prompt("bye", TONE_BYE);
    for (int i = 0; i < 50 && !app_audio_idle(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    /* 刷屏中途断电会让墨水屏停在半刷状态，等它刷完 */
    for (int i = 0; i < 300 && s_disp_busy; i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (s_dirty) export_progress();
    epaper_port_sleep();
    app_power_off();
}

/* 刷屏期间仍然可以执行的命令：只出声、不换画面 */
static bool audio_only(int cmd) { return cmd == SR_CMD_REPEAT || cmd == SR_CMD_WORDS; }

static void handle_learn_cmd(int cmd)
{
    if (s_disp_busy && !audio_only(cmd)) {
        ESP_LOGI(TAG, "正在刷屏，命令 %d 不执行", cmd);
        int64_t now = esp_timer_get_time();
        if (now - s_busy_said > BUSY_PROMPT_GAP_US) {
            s_busy_said = now;
            app_audio_prompt("busy", TONE_BUSY);
        }
        return;
    }
    int id = cur_id();
    if (id < 0) return;
    if (s_show_at && cmd == SR_CMD_NEXT) { show(); return; }   /* 换组后还没呈现：下一个就是本组第一个字 */

    if (cmd >= SR_CMD_GROUP_BASE) {
        int g = cmd - SR_CMD_GROUP_BASE;               /* 从 1 开始 */
        if (g < 1 || g > app_groups_count()) { ESP_LOGW(TAG, "没有第 %d 组", g); return; }
        switch_group(g - 1);
        return;
    }
    if (cmd >= SR_CMD_CHAR_BASE) {                     /* 「如果的如」→ 清单第 N 个字 */
        int idx = cmd - SR_CMD_CHAR_BASE;
        if (idx >= s_scope->n) return;
        ESP_LOGI(TAG, "跳到本组第 %d 个字", idx + 1);
        goto_pos(idx);
        return;
    }

    switch (cmd) {
    case SR_CMD_NEXT:   goto_pos(s_pos + 1); break;
    case SR_CMD_PREV:   goto_pos(s_pos - 1); break;
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
        if (changed) { mark_dirty(); request_display(id); }   /* 去掉红勾 */
        break;
    }
    case SR_CMD_GROUP_NEXT: switch_group(s_group + 1); break;
    case SR_CMD_GROUP_PREV: switch_group(s_group - 1); break;
    default: break;
    }
}

static void handle_cmd(int cmd)
{
    switch (cmd) {
    case EVT_SR_READY:
        s_sr_ready = true;
        sr_update_scope();
        ESP_LOGI(TAG, "语音就绪 —— 说「你好小智」唤醒");
        return;
    case APP_EVT_STATUS:    log_status(); return;
    case APP_EVT_LIST:      log_scope(); return;
    case APP_EVT_EXPORT:    export_progress(); return;
    case APP_EVT_POWER_OFF: power_off("长按 PWR"); return;
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
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(s_show_at ? 100 : 1000)) == pdTRUE) handle_cmd(cmd);

        int64_t now = esp_timer_get_time();
        if (s_show_at) {                                  /* 换组后推迟的呈现：提示音放完才开始计时 */
            if (!app_audio_idle()) s_show_at = now + defer_us();
            else if (now >= s_show_at) show();
        }
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
/* BOOT: 单击 下一个   双击 重播字音   长按 读词组
 * PWR : 单击 我会了   双击 复习       长按 关机 */
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
        else if (get_bit_button(b, BOOT_DOUBLE)) { ESP_LOGI(TAG, "双击 BOOT → 重播");   post_cmd(SR_CMD_REPEAT); }
        else if (get_bit_button(b, BOOT_SINGLE)) { ESP_LOGI(TAG, "单击 BOOT → 下一个"); post_cmd(SR_CMD_NEXT); }

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
    app_power_init();                    /* 电池自锁 + 屏幕/音频电源域 */
    app_store_init();                    /* NVS 进度 */

    i2c_master_Init();
    user_button_init();
    audio_bsp_init();
    audio_play_init();                   /* 16kHz / 2ch / 16bit，播放+录音同时打开 */

    /* 注意 _sdcard_init() 成功返回 1、失败返回 0，与常见约定相反 */
    if (_sdcard_init() != 1) ESP_LOGE(TAG, "SD 卡初始化失败 —— 请确认已插卡且为 FAT32");

    app_store_load_config(CONFIG_PATH, &s_cfg);
    app_audio_init(SD_ROOT);
    app_audio_set_volume(s_cfg.volume);

    s_epd_buf = (uint8_t *)heap_caps_malloc(EPD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    assert(s_epd_buf);
    epaper_port_init();

    s_total = load_total();
    if (s_total <= 0) {
        ESP_LOGE(TAG, "没有可用生字数据");
        app_audio_tone(TONE_TIMEOUT);
        epaper_port_clear(EPD_1IN54G_WHITE);
        app_store_set_shown(-1);
        if (!app_power_usb_connected()) {        /* 电池供电就别空耗电 */
            vTaskDelay(pdMS_TO_TICKS(2000));
            epaper_port_sleep();
            app_power_off();
        }
        return;
    }

    int cap = s_total > SR_MAX_SCOPE_CHARS ? s_total : SR_MAX_SCOPE_CHARS;
    s_scope     = scope_new(cap);
    s_scope_alt = scope_new(cap);
    assert(s_scope && s_scope_alt);
    app_groups_scan(SCOPE_DIR);
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
