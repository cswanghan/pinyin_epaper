/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 数据流:
 *   麦克风 --audio_playback_read--> feed 任务 --afe->feed--> AFE 前端
 *                                                             |
 *   回调 <-- detect 任务 <--afe->fetch-- 唤醒检测 / 命令词识别 --+
 *
 * 关键设计:
 *   1. 板载喇叭和麦克风距离极近，朗读时声音必然被拾取。用 app_sr_set_muted()
 *      在播放期间给 AFE 喂静音，避免自激误触发。（比 AEC 简单可靠，
 *      因为我们拿不到播放端的参考信号。）
 *   2. 命令词按学习清单分组注册（400 条上限），切组时运行中重新注册。
 *      esp-sr 的命令词接口不是线程安全的，detect 与重新注册之间用互斥锁隔开；
 *      detect 拿不到锁就丢弃这一帧，保证 AFE 的输出一直被取走、不会堵塞。
 *   3. 连续对话：命令执行后继续听 follow_up_ms，不用每次都喊唤醒词。
 *      播放期间暂停计时，读完字音后孩子仍有完整的时间说下一句。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "app_sr.h"
#include "audio_bsp.h"

#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

#define TAG "SR"

#define SR_MAX_PHRASES   400        /* MultiNet7 单次最多注册的说法条数 */

static const esp_afe_sr_iface_t *s_afe   = NULL;
static esp_afe_sr_data_t        *s_afe_d = NULL;
static const esp_mn_iface_t     *s_mn    = NULL;
static model_iface_data_t       *s_mn_d  = NULL;

static sr_cmd_cb_t       s_cb    = NULL;
static sr_config_t       s_cfg   = { false, 0.0f, 6000, 6000 };
static SemaphoreHandle_t s_lock  = NULL;     /* 保护 MultiNet（detect / 重新注册）*/
static volatile bool     s_muted = false;
static int               s_phrase_n = 0;     /* 本轮已注册条数，用于截断 */

void app_sr_set_muted(bool muted) { s_muted = muted; }

/* ---------- 固定命令词 ----------
 * 无声调拼音，空格分隔；同一命令的多种说法用逗号分隔。
 * 多挂几种说法能明显提高小孩说话的命中率。
 * 「跳过」归入下一个（不标记掌握）；去掉了「下一字」（和「下一组」太像）。
 */
struct fixed_cmd { int id; const char *phrases; };
static const struct fixed_cmd FIXED[] = {
    { SR_CMD_NEXT,       "xia yi ge,huan yi ge,xia yi ti,tiao guo" },
    { SR_CMD_PREV,       "shang yi ge,hui qu,gang cai na ge" },
    { SR_CMD_REPEAT,     "zai du yi bian,zai shuo yi bian,mei ting qing,zai lai yi ci" },
    { SR_CMD_WORDS,      "du ci zu,zen me zu ci,zu ge ci,you shen me ci" },
    { SR_CMD_MASTERED,   "wo hui le,hui le,ren shi" },
    { SR_CMD_REVIEW,     "fu xi,bu hui de,cuo de" },
    { SR_CMD_FORGOT,     "wang le,wo wang le,mei ji zhu" },
    { SR_CMD_GROUP_NEXT, "xia yi zu,huan yi zu" },
    { SR_CMD_GROUP_PREV, "shang yi zu" },
};

/* 把 "a,b,c" 拆开逐条注册，超过总上限就停
 *
 * 必须用 strtok_r：esp_mn_commands_add 内部会调用同样使用 strtok 的函数
 * （flite_g2p / check_speech_command），用非重入版本会让内部状态被冲掉，
 * 结果每条命令只有第一种说法被注册，且不报任何错误。
 */
static int add_phrases(int cmd_id, const char *csv)
{
    char buf[256];
    strncpy(buf, csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ') tok++;               /* 去掉前导空格 */
        if (!*tok) continue;
        if (s_phrase_n >= SR_MAX_PHRASES) break;
        if (esp_mn_commands_add(cmd_id, tok) == ESP_OK) { n++; s_phrase_n++; }
    }
    return n;
}

/* 数字 → 无声调拼音（1-99），用于「第N组」*/
static void num_pinyin(int n, char *out, size_t sz)
{
    static const char *D[] = { "ling", "yi", "er", "san", "si", "wu", "liu", "qi", "ba", "jiu" };
    if (n < 10)             snprintf(out, sz, "%s", D[n]);
    else if (n == 10)       snprintf(out, sz, "shi");
    else if (n < 20)        snprintf(out, sz, "shi %s", D[n % 10]);
    else if (n % 10 == 0)   snprintf(out, sz, "%s shi", D[n / 10]);
    else                    snprintf(out, sz, "%s shi %s", D[n / 10], D[n % 10]);
}

int app_sr_set_scope(const char *const *phrases, int n, int group_count)
{
    if (!s_mn_d || !s_lock) { ESP_LOGE(TAG, "MultiNet 未初始化"); return -1; }
    if (n > SR_MAX_SCOPE_CHARS) n = SR_MAX_SCOPE_CHARS;
    if (group_count > SR_MAX_GROUPS) group_count = SR_MAX_GROUPS;

    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    esp_mn_commands_clear();
    s_phrase_n = 0;
    int n_fixed = 0, n_group = 0, n_char = 0;
    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++)
        n_fixed += add_phrases(FIXED[i].id, FIXED[i].phrases);

    for (int g = 1; g <= group_count; g++) {
        char num[24], ph[40];
        num_pinyin(g, num, sizeof(num));
        snprintf(ph, sizeof(ph), "di %s zu", num);
        n_group += add_phrases(SR_CMD_GROUP_BASE + g, ph);
    }

    int n_skipped = 0;
    for (int i = 0; i < n; i++) {
        if (!phrases || !phrases[i] || !phrases[i][0]) continue;
        if (s_phrase_n >= SR_MAX_PHRASES) { n_skipped++; continue; }
        n_char += add_phrases(SR_CMD_CHAR_BASE + i, phrases[i]);
    }

    /* 返回的是"模型无法接受的短语"列表，而非错误码 —— 正好用来暴露拼音写错的命令词 */
    esp_mn_error_t *err = esp_mn_commands_update();
    int rejected = (err && err->num > 0) ? err->num : 0;
    s_mn->clean(s_mn_d);
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "命令词: 固定 %d + 选组 %d + 跳字 %d = %d 条（%d 字）, 被拒 %d, 耗时 %d ms",
             n_fixed, n_group, n_char, s_phrase_n, n, rejected,
             (int)((esp_timer_get_time() - t0) / 1000));
    if (n_skipped)
        ESP_LOGW(TAG, "超过 %d 条上限，%d 个字没有跳字命令", SR_MAX_PHRASES, n_skipped);
    for (int i = 0; i < rejected && i < 10; i++) {
        if (err->phrases[i])
            ESP_LOGW(TAG, "  模型不接受: \"%s\"", err->phrases[i]->string);
    }
    return rejected;
}

/* ---------- feed 任务：麦克风 → AFE ---------- */
static void feed_task(void *arg)
{
    int chunk    = s_afe->get_feed_chunksize(s_afe_d);       /* 每通道采样数 */
    int feed_ch  = s_afe->get_feed_channel_num(s_afe_d);
    /* codec 以双声道打开，需要把读到的立体声降成 AFE 要的通道数 */
    int16_t *stereo = (int16_t *)heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *feed   = (int16_t *)heap_caps_malloc(chunk * feed_ch * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    assert(stereo && feed);
    ESP_LOGI(TAG, "feed 任务启动: chunk=%d feed_ch=%d", chunk, feed_ch);

    for (;;) {
        audio_playback_read(stereo, chunk * 2 * sizeof(int16_t));
        if (s_muted) {
            /* 朗读中：喂静音，既保持 AFE 时序又不会被喇叭声误触发 */
            memset(feed, 0, chunk * feed_ch * sizeof(int16_t));
        } else {
            for (int i = 0; i < chunk; i++)          /* 取左声道 */
                for (int c = 0; c < feed_ch; c++)
                    feed[i * feed_ch + c] = stereo[i * 2];
        }
        s_afe->feed(s_afe_d, feed);
    }
}

/* ---------- detect 任务：唤醒 + 命令词 ----------
 * IDLE   只看唤醒词
 * LISTEN 送 MultiNet 识别，截止时间到了回 IDLE
 *        唤醒后没说任何命令就超时 → 回调 SR_EVT_TIMEOUT（提示音）
 *        连续对话窗口结束 → 静默回 IDLE
 */
static void detect_task(void *arg)
{
    ESP_LOGI(TAG, "detect 任务启动: mn_chunk=%d afe_chunk=%d",
             s_mn->get_samp_chunksize(s_mn_d), s_afe->get_fetch_chunksize(s_afe_d));

    bool    listening  = false;
    bool    got_cmd    = false;   /* 本次唤醒后是否已识别到命令 */
    bool    need_clean = false;
    int64_t deadline   = 0;

    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_d);
        if (!res || res->ret_value == ESP_FAIL) continue;
        int64_t now = esp_timer_get_time();

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "★ 唤醒（你好小智） 音量 %.1f dB", res->data_volume);
            listening  = true;
            got_cmd    = false;
            need_clean = true;
            deadline   = now + (int64_t)s_cfg.listen_ms * 1000;
            if (s_cb) s_cb(SR_EVT_WAKE);
            continue;
        }
        if (!listening) continue;

        int window_ms = got_cmd ? s_cfg.follow_up_ms : s_cfg.listen_ms;
        if (s_muted) {
            /* 播放中不计时：放完后重新给满一个窗口 */
            deadline   = now + (int64_t)window_ms * 1000;
            need_clean = true;
            continue;
        }
        if (xSemaphoreTake(s_lock, 0) != pdTRUE) continue;   /* 正在切换命令词 */

        if (need_clean) { s_mn->clean(s_mn_d); need_clean = false; }
        esp_mn_state_t st = s_mn->detect(s_mn_d, res->data);
        int cmd = 0;
        bool hit = false;
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_d);
            if (r && r->num > 0) {
                hit = true;
                cmd = r->command_id[0];
                ESP_LOGI(TAG, "识别: \"%s\" id=%d prob=%.3f", r->string, cmd, r->prob[0]);
                for (int i = 1; i < r->num; i++)
                    ESP_LOGI(TAG, "  候选%d: id=%d prob=%.3f", i, r->command_id[i], r->prob[i]);
            }
        } else if (st == ESP_MN_STATE_TIMEOUT) {
            /* 超时以本任务的截止时间为准；模型自己的计时从头再来 */
            s_mn->clean(s_mn_d);
        }
        xSemaphoreGive(s_lock);

        if (hit) {
            got_cmd = true;
            if (s_cb) s_cb(cmd);
            if (s_cfg.follow_up_ms > 0) {
                deadline   = now + (int64_t)s_cfg.follow_up_ms * 1000;
                need_clean = true;
            } else {
                listening = false;
                s_afe->reset_buffer(s_afe_d);
            }
        } else if (now > deadline) {
            ESP_LOGI(TAG, "%s", got_cmd ? "连续对话结束，回到待唤醒" : "没听到命令，回到待唤醒");
            if (!got_cmd && s_cb) s_cb(SR_EVT_TIMEOUT);
            listening = false;
            s_afe->reset_buffer(s_afe_d);
        }
    }
}

/* ---------- 初始化 ---------- */
bool app_sr_start(sr_cmd_cb_t cb, const sr_config_t *cfg)
{
    s_cb = cb;
    if (cfg) s_cfg = *cfg;
    if (s_cfg.listen_ms < 2000) s_cfg.listen_ms = 2000;
    if (s_cfg.follow_up_ms < 0) s_cfg.follow_up_ms = 0;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return false;

    srmodel_list_t *models = esp_srmodel_init("model");   /* 对应分区表里的 model 分区 */
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "未找到语音模型 —— 确认 model 分区已烧录");
        return false;
    }
    for (int i = 0; i < models->num; i++)
        ESP_LOGI(TAG, "模型[%d]: %s", i, models->model_name[i]);

    /* 单麦输入。不启用 AEC：拿不到播放端参考信号，改用播放期间静音输入的办法。*/
    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_cfg) { ESP_LOGE(TAG, "afe_config_init 失败"); return false; }
    afe_cfg->aec_init = false;
    ESP_LOGI(TAG, "WakeNet 默认模式 %d（0=DET_MODE_90 常规, 1=DET_MODE_95 灵敏）",
             (int)afe_cfg->wakenet_mode);
    if (s_cfg.wake_high) afe_cfg->wakenet_mode = DET_MODE_95;

    s_afe   = esp_afe_handle_from_config(afe_cfg);
    s_afe_d = s_afe->create_from_config(afe_cfg);
    if (!s_afe_d) { ESP_LOGE(TAG, "AFE 创建失败"); return false; }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) { ESP_LOGE(TAG, "没有中文命令词模型"); return false; }
    s_mn = esp_mn_handle_from_name(mn_name);
    int mn_timeout = s_cfg.listen_ms;
    if (s_cfg.follow_up_ms > mn_timeout) mn_timeout = s_cfg.follow_up_ms;
    if (mn_timeout < 6000) mn_timeout = 6000;
    s_mn_d = s_mn->create(mn_name, mn_timeout);
    if (!s_mn_d) { ESP_LOGE(TAG, "MultiNet 创建失败"); return false; }
    esp_mn_commands_alloc(s_mn, s_mn_d);          /* 命令词表绑定到这个模型实例 */
    if (s_cfg.mn_threshold > 0.0f) s_mn->set_det_threshold(s_mn_d, s_cfg.mn_threshold);
    ESP_LOGI(TAG, "命令词模型: %s  唤醒模式=%s  阈值=%s%.2f  等命令 %d ms  连续对话 %d ms",
             mn_name, s_cfg.wake_high ? "灵敏" : "常规",
             s_cfg.mn_threshold > 0.0f ? "" : "默认/", s_cfg.mn_threshold,
             s_cfg.listen_ms, s_cfg.follow_up_ms);

    xTaskCreatePinnedToCore(feed_task,   "sr_feed",   4 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8 * 1024, NULL, 5, NULL, 1);
    return true;
}
