/*****************************************************************************
 * 数据层实现
 *
 * NVS 命名空间 "pinyin":
 *   group     str   当前学习组（清单文件名，按名字而不是序号存：家长增删清单后仍能对上）
 *   char      i32   当前字 id（恢复时在清单里重新查找，找不到就从第 1 个开始）
 *   mastered  blob  掌握位图 512 字节，第 id 位 = 1 表示"我会了"
 *   shown     i32   屏幕上现在的画面 = id*2 + 是否画了红勾；-1 未知
 *                   墨水屏断电后仍保留画面，开机对得上就不用再刷 20 秒
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app_store.h"

#define TAG "STORE"
#define NVS_NS "pinyin"

static nvs_handle_t s_nvs = 0;
static uint8_t      s_bits[STORE_MAX_IDS / 8];
static int          s_bits_n  = 0;
static char         s_group[SCOPE_NAME_LEN] = "";
static int          s_char    = -1;
static int          s_shown   = -1;

/* ---------- config.txt ---------- */

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void app_store_load_config(const char *path, app_config_t *c)
{
    c->volume            = 80;
    c->wake_high         = false;
    c->mn_threshold      = 0.0f;
    c->listen_seconds    = 6;
    c->follow_up_seconds = 6;
    c->sleep_minutes     = 10;

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGI(TAG, "没有 %s，全部用默认值", path);
    } else {
        char line[160];
        bool first = true;
        while (fgets(line, sizeof(line), f)) {
            char *p = line;
            if (first && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
                p += 3;                                  /* Windows 记事本的 BOM */
            first = false;
            char *hash = strchr(p, '#');
            if (hash) *hash = '\0';                      /* 允许行尾注释 */
            char *eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char *k = trim(p), *v = trim(eq + 1);
            if (!*k || !*v) continue;

            if      (!strcmp(k, "volume"))            c->volume = clampi(atoi(v), 0, 100);
            else if (!strcmp(k, "wake_sensitivity"))  c->wake_high = !strcasecmp(v, "high");
            else if (!strcmp(k, "mn_threshold")) {
                float t = strtof(v, NULL);
                c->mn_threshold = (t > 0.0f && t < 1.0f) ? t : 0.0f;
            }
            else if (!strcmp(k, "listen_seconds"))    c->listen_seconds    = clampi(atoi(v), 2, 30);
            else if (!strcmp(k, "follow_up_seconds")) c->follow_up_seconds = clampi(atoi(v), 0, 30);
            else if (!strcmp(k, "sleep_minutes"))     c->sleep_minutes     = clampi(atoi(v), 0, 240);
            else ESP_LOGW(TAG, "config.txt 不认识的配置项: %s", k);
        }
        fclose(f);
    }
    ESP_LOGI(TAG, "配置: 音量 %d  唤醒 %s  命令阈值 %.2f  等命令 %ds  连续对话 %ds  自动关机 %d 分钟",
             c->volume, c->wake_high ? "high" : "normal", c->mn_threshold,
             c->listen_seconds, c->follow_up_seconds, c->sleep_minutes);
}

/* ---------- NVS ---------- */

static int popcount_bits(void)
{
    int n = 0;
    for (size_t i = 0; i < sizeof(s_bits); i++) n += __builtin_popcount(s_bits[i]);
    return n;
}

bool app_store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区需要擦除重建（%s）", esp_err_to_name(err));
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err == ESP_OK) err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 不可用（%s），进度不会保存", esp_err_to_name(err));
        s_nvs = 0;
        return false;
    }

    size_t sz = sizeof(s_group);
    if (nvs_get_str(s_nvs, "group", s_group, &sz) != ESP_OK) s_group[0] = '\0';
    int32_t v;
    s_char  = (nvs_get_i32(s_nvs, "char", &v) == ESP_OK) ? v : -1;
    s_shown = (nvs_get_i32(s_nvs, "shown", &v) == ESP_OK) ? v : -1;
    sz = sizeof(s_bits);
    if (nvs_get_blob(s_nvs, "mastered", s_bits, &sz) != ESP_OK) memset(s_bits, 0, sizeof(s_bits));
    s_bits_n = popcount_bits();

    ESP_LOGI(TAG, "进度: 组=\"%s\" 字id=%d 已会 %d 字 屏幕=%d", s_group, s_char, s_bits_n, s_shown);
    return true;
}

void app_store_get_pos(char *group, size_t sz, int *char_id)
{
    strncpy(group, s_group, sz - 1);
    group[sz - 1] = '\0';
    *char_id = s_char;
}

void app_store_save_pos(const char *group, int char_id)
{
    bool dirty = false;
    if (strcmp(group, s_group) != 0) {
        strncpy(s_group, group, sizeof(s_group) - 1);
        if (s_nvs) nvs_set_str(s_nvs, "group", s_group);
        dirty = true;
    }
    if (char_id != s_char) {
        s_char = char_id;
        if (s_nvs) nvs_set_i32(s_nvs, "char", s_char);
        dirty = true;
    }
    if (dirty && s_nvs) nvs_commit(s_nvs);
}

bool app_store_is_mastered(int id)
{
    return id >= 0 && id < STORE_MAX_IDS && (s_bits[id / 8] >> (id % 8)) & 1;
}

bool app_store_set_mastered(int id, bool on)
{
    if (id < 0 || id >= STORE_MAX_IDS || app_store_is_mastered(id) == on) return false;
    if (on) s_bits[id / 8] |=  (uint8_t)(1 << (id % 8));
    else    s_bits[id / 8] &= (uint8_t)~(1 << (id % 8));
    s_bits_n += on ? 1 : -1;
    if (s_nvs) {
        nvs_set_blob(s_nvs, "mastered", s_bits, sizeof(s_bits));
        nvs_commit(s_nvs);
    }
    return true;
}

int app_store_mastered_count(void) { return s_bits_n; }
int app_store_get_shown(void)      { return s_shown; }

void app_store_set_shown(int code)
{
    if (code == s_shown) return;
    s_shown = code;
    if (s_nvs) {
        nvs_set_i32(s_nvs, "shown", code);
        nvs_commit(s_nvs);
    }
}

/* ---------- 学习清单列表 ---------- */

static char (*s_groups)[SCOPE_NAME_LEN] = NULL;
static int   s_group_n = 0;

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int app_groups_scan(const char *dir)
{
    if (!s_groups)
        s_groups = (char (*)[SCOPE_NAME_LEN])heap_caps_calloc(GROUPS_MAX, SCOPE_NAME_LEN, MALLOC_CAP_SPIRAM);
    s_group_n = 0;
    DIR *d = s_groups ? opendir(dir) : NULL;
    if (!d) { ESP_LOGW(TAG, "打不开清单目录 %s", dir); return 0; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        size_t len = strlen(nm);
        if (nm[0] == '.' || e->d_type == DT_DIR) continue;     /* 跳过 macOS 的 ._xxx */
        if (len < 5 || len >= SCOPE_NAME_LEN || strcasecmp(nm + len - 4, ".txt") != 0) continue;
        if (s_group_n >= GROUPS_MAX) { ESP_LOGW(TAG, "清单超过 %d 个，其余忽略", GROUPS_MAX); break; }
        strcpy(s_groups[s_group_n++], nm);
    }
    closedir(d);
    qsort(s_groups, s_group_n, SCOPE_NAME_LEN, cmp_name);   /* 01_、02_ … 文件名前缀决定顺序 */
    ESP_LOGI(TAG, "学习清单 %d 组", s_group_n);
    return s_group_n;
}

int         app_groups_count(void)     { return s_group_n; }
const char *app_groups_name(int g)     { return (g >= 0 && g < s_group_n) ? s_groups[g] : ""; }

int app_groups_find(const char *name)
{
    for (int g = 0; g < s_group_n; g++)
        if (strcmp(s_groups[g], name) == 0) return g;
    return -1;
}

/* ---------- 清单解析 ---------- */

scope_t *scope_new(int cap)
{
    scope_t *s = (scope_t *)heap_caps_calloc(1, sizeof(scope_t), MALLOC_CAP_SPIRAM);
    if (!s) return NULL;
    s->cap     = cap;
    s->buf_cap = (size_t)cap * 48 + 4096;
    s->ids     = (int *)heap_caps_calloc(cap, sizeof(int), MALLOC_CAP_SPIRAM);
    s->chars   = (const char **)heap_caps_calloc(cap, sizeof(char *), MALLOC_CAP_SPIRAM);
    s->phrases = (const char **)heap_caps_calloc(cap, sizeof(char *), MALLOC_CAP_SPIRAM);
    s->buf     = (char *)heap_caps_malloc(s->buf_cap, MALLOC_CAP_SPIRAM);
    if (!s->ids || !s->chars || !s->phrases || !s->buf) { scope_free(s); return NULL; }
    return s;
}

void scope_free(scope_t *s)
{
    if (!s) return;
    free(s->ids); free(s->chars); free(s->phrases); free(s->buf);
    free(s);
}

/* 格式（gen_scopes.py 生成，家长也可手写）: <字id>\t<汉字>\t<说法,备选说法>
 * 整个文件读进 buf 后原地切分，chars / phrases 直接指向 buf。*/
bool scope_load(scope_t *s, const char *dir, const char *name, int total)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "打不开清单: %s", path); return false; }
    size_t len = fread(s->buf, 1, s->buf_cap - 1, f);
    bool truncated = (len == s->buf_cap - 1) && fgetc(f) != EOF;
    fclose(f);
    s->buf[len] = '\0';
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->name[sizeof(s->name) - 1] = '\0';

    char *p = s->buf;
    if ((uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF) p += 3;
    int n = 0, bad = 0;
    while (*p && n < s->cap) {
        char *line = p;
        char *nl = strchr(p, '\n');
        if (nl) { *nl = '\0'; p = nl + 1; } else { p += strlen(p); }
        line[strcspn(line, "\r")] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *t1 = strchr(line, '\t');
        char *ch = t1 ? t1 + 1 : (char *)"";
        if (t1) *t1 = '\0';
        char *t2 = strchr(ch, '\t');
        const char *phr = "";
        if (t2) { *t2 = '\0'; phr = t2 + 1; }

        char *end;
        long id = strtol(line, &end, 10);
        if (end == line || id < 0 || id >= total || id >= STORE_MAX_IDS) { bad++; continue; }
        s->ids[n] = (int)id;
        s->chars[n] = ch;
        s->phrases[n] = phr;
        n++;
    }
    s->n = n;
    if (truncated || *p) ESP_LOGW(TAG, "清单 %s 太长，只用了前 %d 字", name, n);
    if (bad)             ESP_LOGW(TAG, "清单 %s 有 %d 行格式不对或字 id 越界，已跳过", name, bad);
    return n > 0;
}

/* 没有任何清单时的兜底：按字表顺序学全部字（没有跳字命令）*/
void scope_fill_all(scope_t *s, int total)
{
    s->name[0] = '\0';
    s->n = total < s->cap ? total : s->cap;
    for (int i = 0; i < s->n; i++) {
        s->ids[i] = i;
        s->chars[i] = "";
        s->phrases[i] = "";
    }
}

/* ---------- progress.txt ---------- */

static void write_list(FILE *f, const scope_t *s, bool mastered)
{
    fputs(mastered ? "  已会: " : "  未会: ", f);
    for (int i = 0; i < s->n; i++) {
        if (app_store_is_mastered(s->ids[i]) != mastered) continue;
        if (s->chars[i][0]) fputs(s->chars[i], f);
        else                fprintf(f, "#%d ", s->ids[i]);
    }
    fputc('\n', f);
}

bool app_store_export_progress(const char *path, const char *scope_dir, int total,
                               const char *cur_group, const char *cur_char)
{
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { ESP_LOGW(TAG, "写不了 %s", tmp); return false; }

    fprintf(f, "# 学习进度 —— 设备自动生成，只写不读（改了也没用，进度存在设备里）\n");
    fprintf(f, "已会 %d / %d 字\n", s_bits_n, total);
    fprintf(f, "当前: %s 「%s」\n\n", cur_group[0] ? cur_group : "(全部字)", cur_char);

    scope_t *s = s_group_n ? scope_new(total > SCOPE_NAME_LEN ? total : SCOPE_NAME_LEN) : NULL;
    for (int g = 0; s && g < s_group_n; g++) {
        if (!scope_load(s, scope_dir, s_groups[g], total)) continue;
        int k = 0;
        for (int i = 0; i < s->n; i++) k += app_store_is_mastered(s->ids[i]);
        fprintf(f, "%s  %d/%d%s\n", s_groups[g], k, s->n, k == s->n ? "  ✓" : "");
        write_list(f, s, true);
        write_list(f, s, false);
        fputc('\n', f);
    }
    scope_free(s);

    bool ok = (fclose(f) == 0);
    if (ok) {
        unlink(path);                      /* FATFS 的 rename 不会覆盖已存在的文件 */
        ok = (rename(tmp, path) == 0);
    }
    ESP_LOGI(TAG, "导出进度 %s: %s", path, ok ? "完成" : "失败");
    return ok;
}
