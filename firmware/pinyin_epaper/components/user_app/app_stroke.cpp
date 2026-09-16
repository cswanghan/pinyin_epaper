/*****************************************************************************
 * 笔顺朗读 —— 详见 app_stroke.h
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "app_stroke.h"
#include "app_audio.h"

#define TAG "STROKE"

/* 下面三个常量必须和 content/gen_stroke.py 对齐，改一个就要改两边 */
#define CLIP_N      29        /* 笔画名片段 s00..s28 = STROKE_NAMES 的长度 */
#define NUM_N       24        /* 「N 画」片段 n01..n23，下标 0 空着 */
#define SHORT_MAX   4         /* ≤ 这个画数的字，笔顺念两遍（不然念完还剩十几秒干等）*/

/* 刷屏窗口 = 从开始念笔顺到字显形。念完比刷完早一点更好：孩子先写完、再看到字，
 * 比字先出来再补完笔顺顺。这个值不写死 —— 四色墨水屏的波形时长随温度变，同一块板
 * 实测到过 17.6 秒，也实测到过 20.5 秒。display_task 每刷完一屏就把实测值回填进来
 * （app_stroke_set_window），下面只是开机头一个字还没有实测值时的初值 */
#define WINDOW_DEFAULT_MS 20500
#define GAP_MIN_MS  250       /* 再挤就成连读了，十几画的字宁可超时 */
#define GAP_MAX_MS  2000      /* 一两画的字念两遍还是填不满，留白别太长 */

/* 只是个基准值，不是所有文件的真实头长: ffmpeg 会在 fmt 和 data 之间插 LIST 元数据块，
 * 卡上 aud 目录里的 wav 实测头就是 78 字节（stroke 和 sys 里的是 44）。按 44 算等于把多出
 * 来的 34 字节当成了声音，一个文件差 1 毫秒、head 两个文件差 2 毫秒，对 17.6 秒的分摊无所谓，
 * 所以这里不解析块结构（真正播放的 app_audio.cpp 是老老实实按 RIFF 块找 data 的）*/
#define WAV_HDR     44
#define BYTES_PER_MS 32       /* 16kHz × 16bit × 单声道 = 32 字节/毫秒 */

static char        *s_buf   = NULL;   /* bishun.txt 原文，编码串就指进这里 */
static const char **s_enc   = NULL;   /* 字id → 笔画编码串（"0a5b…"），NULL = 没有 */
static int          s_total = 0;
static char         s_root[64] = "";
static int          s_dur_s[CLIP_N] = {0};
static int          s_dur_n[NUM_N]  = {0};
static int          s_dur_tail = 0, s_dur_again = 0;
static bool         s_ready = false;
static int          s_window_ms = WINDOW_DEFAULT_MS;
static bool         s_measured  = false;   /* 窗口是实测值还是初值，只用于日志 */

/* 编码字符 → 片段编号，和 gen_stroke.py 的 ENC 一致 */
static int dec(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    return -1;
}

/* WAV 时长（毫秒）。只 stat 文件大小，按 WAV_HDR 估，误差见上面那段注释 */
static int wav_ms(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= WAV_HDR) return 0;
    return (int)((st.st_size - WAV_HDR) / BYTES_PER_MS);
}

static int clip_ms(const char *dir, const char *key)
{
    char p[96];
    snprintf(p, sizeof(p), "%s/stroke/%s.wav", dir, key);
    return wav_ms(p);
}

static bool load_table(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGI(TAG, "没有 %s，不念笔顺", path); return false; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) { fclose(f); ESP_LOGW(TAG, "笔顺表大小异常 %ld", sz); return false; }

    s_buf = (char *)heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM);
    s_enc = (const char **)heap_caps_calloc(s_total, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!s_buf || !s_enc) { fclose(f); ESP_LOGE(TAG, "笔顺表内存不够"); return false; }
    size_t n = fread(s_buf, 1, sz, f);
    fclose(f);
    s_buf[n] = '\0';

    /* 格式: <字id>\t<汉字>\t<笔画编码>   —— 原地切分，s_enc 直接指向 s_buf */
    char *p = s_buf;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    int rows = 0, bad = 0;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; } else { p += strlen(p); }
        line[strcspn(line, "\r")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *t1 = strchr(line, '\t');
        if (!t1) { bad++; continue; }
        *t1 = '\0';
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) { bad++; continue; }
        *t2 = '\0';
        char *enc = t2 + 1;

        char *end;
        long id = strtol(line, &end, 10);
        if (end == line || id < 0 || id >= s_total || !*enc) { bad++; continue; }
        s_enc[id] = enc;
        rows++;
    }
    ESP_LOGI(TAG, "笔顺表 %d 字%s", rows, bad ? "" : "");
    if (bad) ESP_LOGW(TAG, "笔顺表有 %d 行格式不对，已跳过", bad);
    return rows > 0;
}

bool app_stroke_init(const char *sd_root, int total)
{
    s_ready = false;
    s_total = total > 0 ? total : 0;
    strncpy(s_root, sd_root, sizeof(s_root) - 1);
    if (!s_total) return false;

    char path[96];
    snprintf(path, sizeof(path), "%s/stroke/bishun.txt", sd_root);
    if (!load_table(path)) return false;

    /* 片段时长开机量一次：之后每个字要算停顿，不能每次都去 SD 上 stat 几十个文件 */
    int have = 0;
    for (int i = 0; i < CLIP_N; i++) {
        char k[8]; snprintf(k, sizeof(k), "s%02d", i);
        if ((s_dur_s[i] = clip_ms(sd_root, k)) > 0) have++;
    }
    for (int i = 1; i < NUM_N; i++) {
        char k[8]; snprintf(k, sizeof(k), "n%02d", i);
        s_dur_n[i] = clip_ms(sd_root, k);
    }
    s_dur_tail  = clip_ms(sd_root, "tail");
    s_dur_again = clip_ms(sd_root, "again");
    if (have < CLIP_N) {
        ESP_LOGW(TAG, "笔画名片段只有 %d/%d 个，缺的那几笔会静音跳过", have, CLIP_N);
        if (!have) return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "笔顺朗读就绪: 笔画名 %d 个，尾句 %d ms", have, s_dur_tail);
    return true;
}

int app_stroke_count(int id)
{
    if (!s_ready || id < 0 || id >= s_total || !s_enc[id]) return -1;
    return (int)strlen(s_enc[id]);
}

void app_stroke_set_window(int ms)
{
    if (ms < 5000 || ms > 40000) return;      /* 不像一次正常刷屏，别拿它当基准 */
    s_window_ms = ms;
    s_measured  = true;
}

int app_stroke_speak(int id, int gap_ms)
{
    if (!s_ready || id < 0 || id >= s_total || !s_enc[id]) return 0;
    const char *enc = s_enc[id];
    int k = (int)strlen(enc);
    bool twice = (k <= SHORT_MAX);            /* 一两画的字念一遍太短，填不住刷屏 */

    /* 先算念完要多久（不含停顿），再把剩下的时间平摊到每个停顿上 */
    int names = 0;
    for (int i = 0; i < k; i++) {
        int c = dec(enc[i]);
        if (c >= 0 && c < CLIP_N) names += s_dur_s[c];
    }
    int body  = (k < NUM_N ? s_dur_n[k] : 0) + names + s_dur_tail;
    int slots = k;
    if (twice) { body += s_dur_again + names; slots += k; }

    int gap = gap_ms;
    if (gap <= 0) {
        /* 字音和词组只有还排在前面时才占这个窗口。插队换字的那个字在上一张刷屏的时候
         * 就报读过了，这会儿队列是空的，head 得算 0，不然会比字显形早念完四五秒。
         * 长度每个字都不一样，直接量 SD 上的文件，比估一个平均值准 */
        int head = 0;
        if (!app_audio_idle()) {
            char p[96];
            snprintf(p, sizeof(p), "%s/aud/%04d_c.wav", s_root, id);
            head = wav_ms(p);
            snprintf(p, sizeof(p), "%s/aud/%04d_w.wav", s_root, id);
            head += wav_ms(p);
        }
        gap = slots ? (s_window_ms - head - body) / slots : GAP_MIN_MS;
        if (gap < GAP_MIN_MS) gap = GAP_MIN_MS;
        if (gap > GAP_MAX_MS) gap = GAP_MAX_MS;
    }

    char p[96];
    if (k < NUM_N && s_dur_n[k] > 0) {
        snprintf(p, sizeof(p), "%s/stroke/n%02d.wav", s_root, k);
        app_audio_play_file(p);
    }
    for (int pass = 0; pass < (twice ? 2 : 1); pass++) {
        if (pass) {
            snprintf(p, sizeof(p), "%s/stroke/again.wav", s_root);
            app_audio_play_file(p);
        }
        for (int i = 0; i < k; i++) {
            int c = dec(enc[i]);
            if (c < 0 || c >= CLIP_N) continue;
            snprintf(p, sizeof(p), "%s/stroke/s%02d.wav", s_root, c);
            app_audio_play_file(p);
            app_audio_gap(gap);
        }
    }
    snprintf(p, sizeof(p), "%s/stroke/tail.wav", s_root);
    app_audio_play_file(p);

    int total_ms = body + gap * slots;
    ESP_LOGI(TAG, "笔顺 %d 画%s 停顿 %d ms 念完约 %.1f 秒（刷屏窗口 %.1f 秒%s）",
             k, twice ? "×2" : "", gap, total_ms / 1000.0, s_window_ms / 1000.0,
             s_measured ? "实测" : "初值");
    return total_ms;
}
