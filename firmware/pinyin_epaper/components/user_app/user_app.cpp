/*****************************************************************************
 * 小学生字学习机 —— 主应用
 *
 * 硬件: Waveshare ESP32-S3-ePaper-1.54G (200x200 四色墨水屏 + ES8311 音频 + SD)
 *
 * SD 卡目录结构（FAT32）:
 *   /sdcard/pinyin/index.txt        第一行 = 生字总数（十进制）
 *   /sdcard/pinyin/img/0000.bin     10000 字节，墨水屏原生缓冲格式
 *   /sdcard/pinyin/aud/0000_c.wav   字音   16kHz/16bit/单声道 PCM
 *   /sdcard/pinyin/aud/0000_w.wav   词组   同上
 *
 * 设计要点:
 *   墨水屏刷新需 15-20 秒，因此屏幕只做静态展示，全部交互靠音频完成。
 *   刷屏期间正好播放朗读，掩盖等待时间。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "user_app.h"
#include "user_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "board_power_bsp.h"
#include "button_bsp.h"
#include "audio_bsp.h"
#include "i2c_bsp.h"
#include "sdcard_bsp.h"
#include "epaper_port.h"
#include "app_sr.h"

#define TAG "PINYIN"

/* ---------- 常量 ---------- */
#define EPD_W            200
#define EPD_H            200
#define EPD_BUF_SIZE     ((EPD_W / 4) * EPD_H)      /* 4 像素/字节 = 10000 */
#define WAV_HEADER_SIZE  44
#define PLAY_CHUNK       1024                        /* 每次写给 codec 的字节数 */
#define MAX_WAV_BYTES    (512 * 1024)                /* 单个音频上限，放 PSRAM */

#define SD_ROOT          "/sdcard/pinyin"

/* 按键事件位
 * 注意: 这些位号由 button_bsp.c 的回调自行定义，与 multi_button.h 的
 *       ButtonEvent 枚举值无关，不能拿枚举值当位号用。
 *       实际映射见 button_bsp.c 的 on_boot_* 回调。
 */
#define BTN_SINGLE   0    /* on_boot_single_click    */
#define BTN_DOUBLE   1    /* on_boot_double_press    */
#define BTN_PRESSUP  2    /* on_boot_pressup_press   */
#define BTN_LONG     3    /* on_boot_longpress_press */

/* ---------- 全局状态 ---------- */
board_power_bsp_t board_div(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);

static uint8_t *s_epd_buf   = NULL;   /* 墨水屏缓冲 10000 字节 */
static uint8_t *s_wav_buf   = NULL;   /* 原始 WAV（单声道） */
static uint8_t *s_play_buf  = NULL;   /* 转成双声道后的播放缓冲 */
static int      s_total     = 0;      /* 生字总数 */
static int      s_cur       = 0;      /* 当前生字序号 */
static volatile bool s_busy = false;  /* 刷屏/播放中，忽略新按键 */

/* ---------- 墨水屏上电 ---------- */
static void epaper_power_up(void)
{
    gpio_config_t cfg = {};
    cfg.intr_type    = GPIO_INTR_DISABLE;
    cfg.mode         = GPIO_MODE_OUTPUT;
    cfg.pin_bit_mask = (0x1ULL << GPIO_NUM_6);
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en   = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
    gpio_set_level(GPIO_NUM_6, 0);          /* 低电平 = 上电 */
    vTaskDelay(pdMS_TO_TICKS(10));
}

/* ---------- 读取生字总数 ---------- */
static int load_total(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/index.txt", SD_ROOT);
    uint8_t buf[32] = {0};
    size_t len = sizeof(buf) - 1;
    if (sdcard_read_file(path, buf, &len) != 0 || len == 0) {
        ESP_LOGE(TAG, "读不到 %s", path);
        return 0;
    }
    buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = '\0';
    int n = atoi((char *)buf);
    ESP_LOGI(TAG, "生字总数: %d", n);
    return n;
}

/* ---------- 显示一个生字 ---------- */
static void show_char(int id)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/img/%04d.bin", SD_ROOT, id);

    size_t len = EPD_BUF_SIZE;
    if (sdcard_read_file(path, s_epd_buf, &len) != 0) {
        ESP_LOGE(TAG, "读图失败: %s", path);
        return;
    }
    if (len != EPD_BUF_SIZE) {
        ESP_LOGW(TAG, "图片尺寸异常 %s: %u 字节（应为 %d）",
                 path, (unsigned)len, EPD_BUF_SIZE);
        return;
    }
    ESP_LOGI(TAG, "刷屏 id=%d（四色屏需 15-20 秒）", id);
    epaper_port_display(s_epd_buf);
}

/* ---------- 播放 WAV ----------
 * SD 上存的是单声道以节省一半空间，而 codec 按双声道打开，
 * 所以这里把每个采样复制成左右两份再送出去。
 */
static void play_wav(const char *path)
{
    size_t len = MAX_WAV_BYTES;
    if (sdcard_read_file(path, s_wav_buf, &len) != 0) {
        ESP_LOGW(TAG, "读音频失败: %s", path);
        return;
    }
    if (len <= WAV_HEADER_SIZE) {
        ESP_LOGW(TAG, "音频过短: %s", path);
        return;
    }

    const int16_t *mono = (const int16_t *)(s_wav_buf + WAV_HEADER_SIZE);
    size_t samples = (len - WAV_HEADER_SIZE) / sizeof(int16_t);
    ESP_LOGI(TAG, "播放 %s  %u 字节 / %.2f 秒",
             path, (unsigned)len, (float)samples / 16000.0f);

    /* 分块做单声道→双声道并送入 codec，避免一次性占用双倍内存 */
    const size_t frames_per_chunk = PLAY_CHUNK / (2 * sizeof(int16_t));
    int16_t *stereo = (int16_t *)s_play_buf;

    for (size_t off = 0; off < samples; off += frames_per_chunk) {
        size_t n = samples - off;
        if (n > frames_per_chunk) n = frames_per_chunk;
        for (size_t i = 0; i < n; i++) {
            int16_t v = mono[off + i];
            stereo[2 * i]     = v;      /* 左 */
            stereo[2 * i + 1] = v;      /* 右 */
        }
        audio_playback_write(stereo, n * 2 * sizeof(int16_t));
    }
}

static void play_char_audio(int id)
{
    char p[64];
    snprintf(p, sizeof(p), "%s/aud/%04d_c.wav", SD_ROOT, id);
    play_wav(p);
}

static void play_words_audio(int id)
{
    char p[64];
    snprintf(p, sizeof(p), "%s/aud/%04d_w.wav", SD_ROOT, id);
    play_wav(p);
}

/* ---------- 音频任务 ----------
 * 墨水屏刷新要 20 秒且是阻塞的，期间 CPU 只是在等 BUSY 引脚。
 * 把播放放到独立任务，让朗读与刷屏并行：按键后立刻出声，
 * 而不是干等 20 秒静默。这是体感上最大的改善。
 */
static QueueHandle_t s_audio_q = NULL;

static void audio_task(void *arg)
{
    char path[80];
    for (;;) {
        if (xQueueReceive(s_audio_q, path, portMAX_DELAY) == pdTRUE) {
            /* 播放期间抑制语音识别：板载喇叭和麦克风离得很近，
             * 不屏蔽的话朗读声会被拾取，造成误唤醒或误识别。*/
            app_sr_set_muted(true);
            play_wav(path);
            vTaskDelay(pdMS_TO_TICKS(150));   /* 等尾音放完再恢复拾音 */
            app_sr_set_muted(false);
        }
    }
}

/* 把播放请求排入队列，立即返回 */
static void audio_request(int id, char kind)
{
    if (!s_audio_q) return;
    char p[80];
    snprintf(p, sizeof(p), "%s/aud/%04d_%c.wav", SD_ROOT, id, kind);
    xQueueSend(s_audio_q, p, 0);
}

/* ---------- 切换到指定生字 ----------
 * 先把音频排队（另一个核上立刻开始播），再刷屏（本任务阻塞 20 秒）。
 */
static void goto_char(int id)
{
    if (s_total <= 0) return;
    if (id < 0)        id = s_total - 1;
    if (id >= s_total) id = 0;
    s_cur = id;

    s_busy = true;
    audio_request(s_cur, 'c');      /* 字音 —— 立刻开始，与刷屏并行 */
    audio_request(s_cur, 'w');      /* 接着读词组，进一步填充等待时间 */
    show_char(s_cur);               /* 阻塞约 20 秒 */
    s_busy = false;
}

/* ---------- 语音命令回调 ----------
 * 在 detect 任务上下文里被调用，所以不能做耗时操作（刷屏要 20 秒），
 * 否则会卡住识别。这里只把意图转成动作，刷屏交给 goto_char 所在的任务。
 */
static QueueHandle_t s_cmd_q = NULL;

static void sr_on_command(int cmd_id)
{
    if (s_cmd_q) xQueueSend(s_cmd_q, &cmd_id, 0);
}

/* 语音命令执行任务 —— 与识别任务解耦，避免刷屏阻塞识别 */
static void command_task(void *arg)
{
    int cmd;
    for (;;) {
        if (xQueueReceive(s_cmd_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (s_busy) { ESP_LOGI(TAG, "忙碌中，忽略语音命令 %d", cmd); continue; }

        if (cmd >= SR_CMD_CHAR_BASE) {
            /* 按内容跳转：「如果的如」→ 清单里第 N 个字 */
            int idx = cmd - SR_CMD_CHAR_BASE;
            int id  = app_sr_scope_char_id(idx);
            if (id >= 0) {
                ESP_LOGI(TAG, "语音跳转 → 清单第 %d 个字 (id=%d)", idx, id);
                goto_char(id);
            }
            continue;
        }
        switch (cmd) {
        case SR_CMD_NEXT:     ESP_LOGI(TAG, "语音：下一个");   goto_char(s_cur + 1); break;
        case SR_CMD_PREV:     ESP_LOGI(TAG, "语音：上一个");   goto_char(s_cur - 1); break;
        case SR_CMD_REPEAT:   ESP_LOGI(TAG, "语音：再读一遍"); audio_request(s_cur, 'c'); break;
        case SR_CMD_WORDS:    ESP_LOGI(TAG, "语音：读词组");   audio_request(s_cur, 'w'); break;
        case SR_CMD_MASTERED: ESP_LOGI(TAG, "语音：我会了（标记功能待实现）"); goto_char(s_cur + 1); break;
        case SR_CMD_REVIEW:   ESP_LOGI(TAG, "语音：复习（待实现）"); break;
        default: break;
        }
    }
}

/* ---------- 按键任务 ---------- */
static void button_task(void *arg)
{
    for (;;) {
        /* 把弹起位也纳入掩码一并清除，否则它会常驻事件组；但不对它做响应 */
        EventBits_t bits = xEventGroupWaitBits(
            boot_groups,
            set_bit_button(BTN_SINGLE) | set_bit_button(BTN_DOUBLE) |
            set_bit_button(BTN_LONG)   | set_bit_button(BTN_PRESSUP),
            pdTRUE, pdFALSE, pdMS_TO_TICKS(200));

        if (bits == 0) continue;
        ESP_LOGI(TAG, "按键事件位: 0x%02X", (unsigned)(bits & 0xFF));

        if (s_busy) {
            ESP_LOGI(TAG, "忙碌中，忽略按键");
            continue;
        }

        /* 按优先级从高到低判断：双击会同时置起单击位（实测 0x07），
         * 长按同理，所以必须先判复合事件，否则会被当成单击。*/
        if (get_bit_button(bits, BTN_LONG)) {
            ESP_LOGI(TAG, "长按 → 播放词组");
            audio_request(s_cur, 'w');      /* 排队即返回，不阻塞按键任务 */
        } else if (get_bit_button(bits, BTN_DOUBLE)) {
            ESP_LOGI(TAG, "双击 → 重播字音");
            audio_request(s_cur, 'c');
        } else if (get_bit_button(bits, BTN_SINGLE)) {
            ESP_LOGI(TAG, "单击 → 下一个生字");
            goto_char(s_cur + 1);
        }
    }
}

/* ---------- 初始化 ---------- */
void user_app_init(void)
{
    ESP_LOGI(TAG, "===== 小学生字学习机启动 =====");

    /* 两路电源域必须分别打开 */
    board_div.POWEER_EPD_ON();
    board_div.POWEER_Audio_ON();

    i2c_master_Init();
    user_button_init();

    /* 音频 */
    audio_bsp_init();
    audio_play_init();                 /* 16kHz / 2ch / 16bit，播放+录音同时打开 */
    audio_playback_set_vol(80);

    /* SD 卡 —— 注意 _sdcard_init() 成功返回 1、失败返回 0，与常见约定相反 */
    if (_sdcard_init() != 1) {
        ESP_LOGE(TAG, "SD 卡初始化失败 —— 请确认已插卡且为 FAT32");
    } else {
        ESP_LOGI(TAG, "SD 卡挂载成功");
    }

    /* 缓冲区统一放 PSRAM（板载 8MB） */
    s_epd_buf  = (uint8_t *)heap_caps_malloc(EPD_BUF_SIZE,   MALLOC_CAP_SPIRAM);
    s_wav_buf  = (uint8_t *)heap_caps_malloc(MAX_WAV_BYTES,  MALLOC_CAP_SPIRAM);
    s_play_buf = (uint8_t *)heap_caps_malloc(PLAY_CHUNK,     MALLOC_CAP_SPIRAM);
    assert(s_epd_buf && s_wav_buf && s_play_buf);

    /* 墨水屏 */
    epaper_power_up();
    epaper_port_init();

    s_total = load_total();
    if (s_total <= 0) {
        ESP_LOGE(TAG, "没有可用生字数据，停在白屏");
        epaper_port_clear(EPD_1IN54G_WHITE);
        return;
    }

    ESP_LOGI(TAG, "按键: 单击=下一个  双击=重播字音  长按=读词组");

    /* 音频任务固定在 core 0，刷屏在 core 1，两者并行互不阻塞 */
    s_audio_q = xQueueCreate(4, 80);
    assert(s_audio_q);
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4 * 1024, NULL, 5, NULL, 0);

    goto_char(0);

    xTaskCreatePinnedToCore(button_task, "button_task", 4 * 1024, NULL, 4, NULL, 1);

    /* ---- 离线语音 ---- */
    s_cmd_q = xQueueCreate(4, sizeof(int));
    assert(s_cmd_q);
    xTaskCreatePinnedToCore(command_task, "cmd_task", 6 * 1024, NULL, 4, NULL, 1);

    if (app_sr_start(sr_on_command)) {
        /* 默认装第 1 组（官方识字写字教学基本字表，按笔画由简到难）*/
        const char *scope = SD_ROOT "/scope/01_基础字1-50.txt";
        if (app_sr_load_scope(scope) > 0) {
            ESP_LOGI(TAG, "语音就绪 —— 说「你好小智」唤醒");
        } else {
            ESP_LOGW(TAG, "清单加载失败，仅固定命令可用: %s", scope);
        }
    } else {
        ESP_LOGW(TAG, "语音识别未启动，按键仍可用");
    }
}
