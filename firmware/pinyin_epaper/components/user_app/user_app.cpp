/*****************************************************************************
 * 小学生字学习机 —— 主应用
 *
 * 硬件: Waveshare ESP32-S3-ePaper-1.54G (200x200 四色墨水屏 + ES8311 音频 + SD)
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
#include "epaper_port.h"
#include "app_sr.h"
#include "app_audio.h"
#include "app_store.h"
#include "app_stroke.h"
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
        app_audio_prompt("busy", TONE_BUSY);
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
    epaper_port_sleep();
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
