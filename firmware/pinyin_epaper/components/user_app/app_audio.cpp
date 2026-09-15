/*****************************************************************************
 * 音频播放任务
 *
 * 墨水屏刷新要 20 秒且是阻塞的。把播放放到独立任务，让朗读与刷屏并行：
 * 按键后立刻出声，而不是干等 20 秒静默。
 *
 * 打断用"代号"而不是标志位：app_audio_stop() 让代号 +1，
 * 正在播的和已排队的旧请求代号对不上，自然作废；之后排的新请求不受影响。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_audio.h"
#include "app_sr.h"
#include "audio_bsp.h"

#define TAG "AUDIO"

#define SAMPLE_RATE     16000
#define PLAY_CHUNK      1024                 /* 每次写给 codec 的字节数（256 个双声道帧）*/
#define MAX_WAV_BYTES   (512 * 1024)         /* 单个音频上限（16 秒），放 PSRAM */
#define QUEUE_LEN       8
#define TAIL_SPEECH_MS  150                  /* 放完后等尾音散掉再恢复拾音 */
#define TAIL_TONE_MS    80
#define TONE_AMP        9800.0f              /* 约 0.3 满幅，比朗读略轻 */

enum { REQ_WAV, REQ_PROMPT, REQ_TONE };
typedef struct {
    uint8_t  kind;
    uint8_t  tone;
    uint32_t gen;
    char     path[96];
} audio_req_t;

static QueueHandle_t     s_q        = NULL;
static uint8_t          *s_wav_buf  = NULL;
static int16_t          *s_play_buf = NULL;
static char              s_root[48] = "/sdcard/pinyin";
static volatile bool     s_playing  = false;
static volatile uint32_t s_gen      = 0;

/* ---------- 提示音 ---------- */
typedef struct { uint16_t hz; uint16_t ms; } note_t;   /* hz=0 为静音间隔，ms=0 结束 */
static const note_t T_WAKE[]    = { {1568, 150}, {0, 0} };
static const note_t T_BUSY[]    = { {480, 120}, {0, 70}, {480, 120}, {0, 0} };
static const note_t T_TIMEOUT[] = { {784, 110}, {523, 160}, {0, 0} };
static const note_t T_OK[]      = { {1047, 80}, {1568, 120}, {0, 0} };
static const note_t T_FORGOT[]  = { {660, 90}, {440, 140}, {0, 0} };
static const note_t T_DONE[]    = { {1047, 100}, {1319, 100}, {1568, 100}, {2093, 220}, {0, 0} };
static const note_t T_GROUP[]   = { {880, 80}, {0, 40}, {880, 80}, {0, 0} };
static const note_t T_BYE[]     = { {1568, 100}, {1047, 100}, {784, 100}, {523, 250}, {0, 0} };
static const note_t *const TONES[TONE_COUNT] = {
    T_WAKE, T_BUSY, T_TIMEOUT, T_OK, T_FORGOT, T_DONE, T_GROUP, T_BYE,
};

static void play_tone(tone_t t, uint32_t gen)
{
    if (t >= TONE_COUNT) return;
    const bool   decay = (t == TONE_WAKE);          /* 「叮」: 指数衰减，像敲铃 */
    const int    ramp  = SAMPLE_RATE / 200;         /* 5ms 起落，避免爆音 */
    const size_t fpc   = PLAY_CHUNK / (2 * sizeof(int16_t));

    for (const note_t *n = TONES[t]; n->ms && gen == s_gen; n++) {
        int   total = n->ms * (SAMPLE_RATE / 1000);
        float ph = 0.0f, dph = 2.0f * (float)M_PI * n->hz / SAMPLE_RATE;
        for (int done = 0; done < total; ) {
            int cnt = total - done;
            if (cnt > (int)fpc) cnt = (int)fpc;
            for (int i = 0; i < cnt; i++) {
                int   k   = done + i;
                float env = decay ? expf(-4.0f * k / total) : 1.0f;
                if (k < ramp)         env *= (float)k / ramp;
                if (total - k < ramp) env *= (float)(total - k) / ramp;
                int16_t v = n->hz ? (int16_t)(TONE_AMP * env * sinf(ph)) : 0;
                ph += dph;
                if (ph > 2.0f * (float)M_PI) ph -= 2.0f * (float)M_PI;
                s_play_buf[2 * i] = s_play_buf[2 * i + 1] = v;
            }
            audio_playback_write(s_play_buf, cnt * 2 * sizeof(int16_t));
            done += cnt;
        }
    }
}

/* ---------- WAV ---------- */

/* sdcard_read_file 不检查缓冲区大小（按文件实际长度写入），这里自己读，超长就截断 */
static bool read_file_max(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(buf, 1, cap, f);
    bool more = (n == cap) && fgetc(f) != EOF;
    fclose(f);
    if (more) ESP_LOGW(TAG, "%s 超过 %u 字节，只播前面部分", path, (unsigned)cap);
    *len = n;
    return n > 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

typedef struct { int channels, rate, bits; size_t off, len; } wav_info_t;

/* 按 RIFF 块结构找 fmt / data。
 * 不能写死跳过 44 字节：ffmpeg 默认会在 fmt 和 data 之间插一个 LIST 元数据块
 * （实测 PCM 从第 78 字节开始），少跳的那段文本会被当成声音播出一声「咔」。*/
static bool wav_parse(const uint8_t *buf, size_t len, wav_info_t *w)
{
    if (len < 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0)
        return false;
    w->channels = 1; w->rate = SAMPLE_RATE; w->bits = 16;
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t sz = le32(buf + off + 4);
        if (memcmp(buf + off, "fmt ", 4) == 0 && sz >= 16 && off + 8 + 16 <= len) {
            w->channels = le16(buf + off + 10);
            w->rate     = (int)le32(buf + off + 12);
            w->bits     = le16(buf + off + 22);
        } else if (memcmp(buf + off, "data", 4) == 0) {
            size_t avail = len - (off + 8);
            w->off = off + 8;
            w->len = sz < avail ? sz : avail;        /* 文件读截断时以实际读到的为准 */
            return true;
        }
        if (sz > len - off - 8) return false;        /* 块长度越界：文件损坏或读截断 */
        off += 8 + sz + (sz & 1);                    /* RIFF 块按偶数字节对齐 */
    }
    return false;
}

/* SD 上存的是单声道以节省一半空间，codec 按双声道打开，
 * 所以分块把每个采样复制成左右两份再送出去。
 * 文件不存在返回 false（quiet_missing 时不打日志，用于可选的语音提示）。*/
static bool play_wav(const char *path, bool quiet_missing, uint32_t gen)
{
    size_t len = 0;
    if (!read_file_max(path, s_wav_buf, MAX_WAV_BYTES, &len)) {
        if (!quiet_missing) ESP_LOGW(TAG, "读音频失败: %s", path);
        return false;
    }
    wav_info_t w;
    if (!wav_parse(s_wav_buf, len, &w) || w.bits != 16 || w.channels < 1 || w.channels > 2) {
        ESP_LOGW(TAG, "不是 16bit 单/双声道 WAV: %s", path);
        return false;
    }
    if (w.rate != SAMPLE_RATE)
        ESP_LOGW(TAG, "%s 采样率 %d，应为 16000（音调会不对）", path, w.rate);

    const int16_t *pcm   = (const int16_t *)(s_wav_buf + w.off);
    const size_t  frames = w.len / (sizeof(int16_t) * w.channels);
    const size_t  fpc    = PLAY_CHUNK / (2 * sizeof(int16_t));
    ESP_LOGI(TAG, "播放 %s  %.2f 秒", path, (float)frames / SAMPLE_RATE);

    for (size_t off = 0; off < frames; off += fpc) {
        if (gen != s_gen) { ESP_LOGI(TAG, "打断 %s", path); break; }
        size_t n = frames - off;
        if (n > fpc) n = fpc;
        for (size_t i = 0; i < n; i++) {
            if (w.channels == 1) {
                s_play_buf[2 * i] = s_play_buf[2 * i + 1] = pcm[off + i];
            } else {
                s_play_buf[2 * i]     = pcm[2 * (off + i)];
                s_play_buf[2 * i + 1] = pcm[2 * (off + i) + 1];
            }
        }
        audio_playback_write(s_play_buf, n * 2 * sizeof(int16_t));
    }
    return true;
}

/* ---------- 播放任务 ---------- */
static void audio_task(void *arg)
{
    audio_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (r.gen != s_gen) continue;                /* 已被 app_audio_stop 作废 */

        s_playing = true;
        /* 播放期间抑制语音识别：喇叭和麦克风离得很近，不屏蔽会误唤醒 */
        app_sr_set_muted(true);
        int tail = TAIL_SPEECH_MS;
        switch (r.kind) {
        case REQ_WAV:
            play_wav(r.path, false, r.gen);
            break;
        case REQ_PROMPT:
            if (!play_wav(r.path, true, r.gen)) { play_tone((tone_t)r.tone, r.gen); tail = TAIL_TONE_MS; }
            break;
        default:
            play_tone((tone_t)r.tone, r.gen);
            tail = TAIL_TONE_MS;
            break;
        }
        /* 队列放空才恢复拾音，连播「字音 + 词组」中间不给麦克风开口子 */
        if (uxQueueMessagesWaiting(s_q) == 0) {
            vTaskDelay(pdMS_TO_TICKS(tail));
            if (uxQueueMessagesWaiting(s_q) == 0) {
                app_sr_set_muted(false);
                s_playing = false;
            }
        }
    }
}

static void enqueue(uint8_t kind, uint8_t tone, const char *path)
{
    if (!s_q) return;
    audio_req_t r = {};
    r.kind = kind;
    r.tone = tone;
    r.gen  = s_gen;
    if (path) strncpy(r.path, path, sizeof(r.path) - 1);
    if (xQueueSend(s_q, &r, 0) != pdTRUE)
        ESP_LOGW(TAG, "播放队列已满，丢弃 %s", path ? path : "提示音");
}

/* ---------- 对外接口 ---------- */
void app_audio_init(const char *sd_root)
{
    if (sd_root) strncpy(s_root, sd_root, sizeof(s_root) - 1);
    s_wav_buf  = (uint8_t *)heap_caps_malloc(MAX_WAV_BYTES, MALLOC_CAP_SPIRAM);
    s_play_buf = (int16_t *)heap_caps_malloc(PLAY_CHUNK, MALLOC_CAP_SPIRAM);
    s_q        = xQueueCreate(QUEUE_LEN, sizeof(audio_req_t));
    assert(s_wav_buf && s_play_buf && s_q);
    /* 固定在 core 0，刷屏在 core 1，两者并行互不阻塞 */
    xTaskCreatePinnedToCore(audio_task, "audio_task", 4 * 1024, NULL, 5, NULL, 0);
}

void app_audio_set_volume(int vol)
{
    if (vol < 0)   vol = 0;
    if (vol > 100) vol = 100;
    audio_playback_set_vol((uint8_t)vol);
}

void app_audio_play_file(const char *path) { enqueue(REQ_WAV, 0, path); }

void app_audio_play_char(int id, char kind)
{
    char p[96];
    snprintf(p, sizeof(p), "%s/aud/%04d_%c.wav", s_root, id, kind);
    enqueue(REQ_WAV, 0, p);
}

void app_audio_prompt(const char *key, tone_t fallback)
{
    char p[96];
    snprintf(p, sizeof(p), "%s/sys/%s.wav", s_root, key);
    enqueue(REQ_PROMPT, (uint8_t)fallback, p);
}

void app_audio_tone(tone_t tone) { enqueue(REQ_TONE, (uint8_t)tone, NULL); }

void app_audio_stop(void)
{
    s_gen++;
    if (s_q) xQueueReset(s_q);
}

bool app_audio_idle(void)
{
    return !s_playing && (!s_q || uxQueueMessagesWaiting(s_q) == 0);
}
