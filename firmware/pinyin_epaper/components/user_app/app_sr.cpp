/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 数据流:
 *   麦克风 --audio_playback_read--> feed 任务 --afe->feed--> AFE 前端
 *                                                             |
 *   回调 <-- detect 任务 <--afe->fetch-- 唤醒检测 / 命令词识别 --+
 *
 * 两个关键设计:
 *   1. 板载喇叭和麦克风距离极近，朗读时声音必然被拾取。用 app_sr_set_muted()
 *      在播放期间丢弃输入，避免自激误触发。（比启用 AEC 简单可靠，
 *      因为我们拿不到播放端的参考信号。）
 *   2. 命令词运行时从 SD 卡清单加载。2500 字全注册会超 400 条上限，
 *      所以按"学习清单"分组，一次只装当前组。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

#define MAX_SCOPE_CHARS  200        /* 单个清单最多字数（每字约 2 条说法，上限 400）*/
#define MN_DETECT_TIMEOUT_MS  6000  /* 唤醒后等待命令词的时间 */

static const esp_afe_sr_iface_t *s_afe   = NULL;
static esp_afe_sr_data_t        *s_afe_d = NULL;
static const esp_mn_iface_t     *s_mn    = NULL;
static model_iface_data_t       *s_mn_d  = NULL;

static sr_cmd_cb_t  s_cb       = NULL;
static volatile bool s_muted   = false;
static volatile bool s_wake    = false;   /* 已唤醒，正在等命令词 */

/* 当前清单：下标 → 生字 id */
static int s_scope_ids[MAX_SCOPE_CHARS];
static int s_scope_n = 0;

void app_sr_set_muted(bool muted) { s_muted = muted; }
int  app_sr_scope_char_id(int i)  { return (i >= 0 && i < s_scope_n) ? s_scope_ids[i] : -1; }

/* ---------- 固定命令词 ----------
 * 无声调拼音，空格分隔；同一命令的多种说法用逗号分隔（由 esp_mn_commands_add 逐条加）。
 * 多挂几种说法能明显提高小孩说话的命中率。
 */
struct fixed_cmd { int id; const char *phrases; };
static const struct fixed_cmd FIXED[] = {
    { SR_CMD_NEXT,     "xia yi ge,huan yi ge,xia yi ti,xia yi zi" },
    { SR_CMD_PREV,     "shang yi ge,hui qu,gang cai na ge" },
    { SR_CMD_REPEAT,   "zai du yi bian,zai shuo yi bian,mei ting qing,zai lai yi ci" },
    { SR_CMD_WORDS,    "du ci zu,zen me zu ci,zu ge ci,you shen me ci" },
    { SR_CMD_MASTERED, "wo hui le,hui le,ren shi,tiao guo" },
    { SR_CMD_REVIEW,   "fu xi,bu hui de,cuo de" },
};

/* 把 "a,b,c" 拆开逐条注册
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
        if (*tok && esp_mn_commands_add(cmd_id, tok) == ESP_OK) n++;
    }
    return n;
}

/* ---------- 加载学习清单 ----------
 * 清单格式（gen_scopes.py 生成）: <字id>\t<汉字>\t<拼音说法,备选说法>
 */
int app_sr_load_scope(const char *path)
{
    if (!s_mn_d) { ESP_LOGE(TAG, "MultiNet 未初始化"); return -1; }

    FILE *f = fopen(path, "r");
    if (!f) { ESP_LOGE(TAG, "打不开清单: %s", path); return -1; }

    esp_mn_commands_clear();
    int total = 0;
    for (size_t i = 0; i < sizeof(FIXED) / sizeof(FIXED[0]); i++)
        total += add_phrases(FIXED[i].id, FIXED[i].phrases);

    s_scope_n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f) && s_scope_n < MAX_SCOPE_CHARS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;

        char *p1 = strchr(line, '\t');  if (!p1) continue;
        char *p2 = strchr(p1 + 1, '\t'); if (!p2) continue;
        *p1 = *p2 = '\0';
        char *phr = p2 + 1;
        phr[strcspn(phr, "\r\n")] = '\0';        /* 去掉行尾换行 */

        s_scope_ids[s_scope_n] = atoi(line);
        total += add_phrases(SR_CMD_CHAR_BASE + s_scope_n, phr);
        s_scope_n++;
    }
    fclose(f);

    /* 返回的是"模型无法接受的短语"列表，而非错误码 —— 正好用来暴露拼音写错的命令词 */
    esp_mn_error_t *err = esp_mn_commands_update();
    int rejected = (err && err->num > 0) ? err->num : 0;
    ESP_LOGI(TAG, "清单 %s: %d 字, 注册 %d 条命令词, 被拒 %d 条",
             path, s_scope_n, total, rejected);
    for (int i = 0; i < rejected && i < 10; i++) {
        if (err->phrases[i])
            ESP_LOGW(TAG, "  模型不接受: \"%s\"", err->phrases[i]->string);
    }
    return s_scope_n;
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

/* ---------- detect 任务：唤醒 + 命令词 ---------- */
static void detect_task(void *arg)
{
    int mn_chunk = s_mn->get_samp_chunksize(s_mn_d);
    int afe_chunk = s_afe->get_fetch_chunksize(s_afe_d);
    ESP_LOGI(TAG, "detect 任务启动: mn_chunk=%d afe_chunk=%d", mn_chunk, afe_chunk);

    int64_t wake_at = 0;
    for (;;) {
        afe_fetch_result_t *res = s_afe->fetch(s_afe_d);
        if (!res || res->ret_value == ESP_FAIL) continue;

        if (!s_wake) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "★ 唤醒（你好小智）");
                s_wake = true;
                wake_at = esp_timer_get_time();
                s_mn->clean(s_mn_d);
            }
            continue;
        }

        /* 已唤醒，送入命令词识别 */
        esp_mn_state_t st = s_mn->detect(s_mn_d, res->data);
        if (st == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = s_mn->get_results(s_mn_d);
            if (r->num > 0) {
                ESP_LOGI(TAG, "识别: id=%d prob=%.2f \"%s\"",
                         r->command_id[0], r->prob[0], r->string);
                if (s_cb) s_cb(r->command_id[0]);
            }
            s_wake = false;
            s_afe->reset_buffer(s_afe_d);
        } else if (st == ESP_MN_STATE_TIMEOUT ||
                   (esp_timer_get_time() - wake_at) > MN_DETECT_TIMEOUT_MS * 1000) {
            ESP_LOGI(TAG, "命令词超时，回到待唤醒");
            s_wake = false;
            s_afe->reset_buffer(s_afe_d);
        }
    }
}

/* ---------- 初始化 ---------- */
bool app_sr_start(sr_cmd_cb_t cb)
{
    s_cb = cb;

    srmodel_list_t *models = esp_srmodel_init("model");   /* 对应分区表里的 model 分区 */
    if (!models || models->num <= 0) {
        ESP_LOGE(TAG, "未找到语音模型 —— 确认 model 分区已烧录");
        return false;
    }
    for (int i = 0; i < models->num; i++)
        ESP_LOGI(TAG, "模型[%d]: %s", i, models->model_name[i]);

    /* 单麦输入。不启用 AEC：拿不到播放端参考信号，改用播放期间静音输入的办法。*/
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) { ESP_LOGE(TAG, "afe_config_init 失败"); return false; }
    cfg->aec_init = false;

    s_afe   = esp_afe_handle_from_config(cfg);
    s_afe_d = s_afe->create_from_config(cfg);
    if (!s_afe_d) { ESP_LOGE(TAG, "AFE 创建失败"); return false; }

    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) { ESP_LOGE(TAG, "没有中文命令词模型"); return false; }
    s_mn   = esp_mn_handle_from_name(mn_name);
    s_mn_d = s_mn->create(mn_name, MN_DETECT_TIMEOUT_MS);
    if (!s_mn_d) { ESP_LOGE(TAG, "MultiNet 创建失败"); return false; }
    ESP_LOGI(TAG, "命令词模型: %s", mn_name);

    xTaskCreatePinnedToCore(feed_task,   "sr_feed",   4 * 1024, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "sr_detect", 8 * 1024, NULL, 5, NULL, 1);
    return true;
}
