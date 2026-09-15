/*****************************************************************************
 * 数据层 —— NVS 进度、config.txt、学习清单、progress.txt 导出
 *
 * 进度存 NVS（flash），不存 SD：拔卡换卡、卡写坏都不丢进度。
 * progress.txt 只是给家长看的导出，设备从不读回。
 *****************************************************************************/
#ifndef APP_STORE_H
#define APP_STORE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORE_MAX_IDS    4096     /* 掌握位图容量（512 字节），字表 2500 字 */
#define SCOPE_NAME_LEN   64
#define GROUPS_MAX       99

/* ---------- config.txt ---------- */
typedef struct {
    int   volume;              /* 0-100 */
    bool  wake_high;           /* wake_sensitivity=high */
    float mn_threshold;        /* 0 = 模型默认 */
    int   listen_seconds;      /* 唤醒后等命令 */
    int   follow_up_seconds;   /* 连续对话，0 = 关 */
    int   sleep_minutes;       /* 无操作自动关机，0 = 不关 */
} app_config_t;

void app_store_load_config(const char *path, app_config_t *cfg);

/* ---------- NVS 进度 ---------- */
bool app_store_init(void);
void app_store_get_pos(char *group, size_t sz, int *char_id);   /* 没存过: "" / -1 */
void app_store_save_pos(const char *group, int char_id);        /* 没变化不写 flash */
bool app_store_is_mastered(int id);
bool app_store_set_mastered(int id, bool on);                   /* 返回是否有变化 */
int  app_store_mastered_count(void);
int  app_store_get_shown(void);         /* 屏幕上现在的画面编码，-1 = 未知 */
void app_store_set_shown(int code);

/* ---------- 学习清单 ---------- */
typedef struct {
    char         name[SCOPE_NAME_LEN];  /* 文件名；"" 表示没有清单时的"全部字" */
    int          n, cap;
    int         *ids;
    const char **chars;                 /* 汉字（UTF-8），指向 buf */
    const char **phrases;               /* 跳字说法，可能是 "" */
    char        *buf;
    size_t       buf_cap;
} scope_t;

int         app_groups_scan(const char *dir);   /* 列出 *.txt 并按文件名排序，返回组数 */
int         app_groups_count(void);
const char *app_groups_name(int g);
int         app_groups_find(const char *name);  /* 找不到返回 -1 */

scope_t *scope_new(int cap);
void     scope_free(scope_t *s);
bool     scope_load(scope_t *s, const char *dir, const char *name, int total);
void     scope_fill_all(scope_t *s, int total);

/* 写 progress.txt（先写 .tmp 再改名，写到一半断电也不会留下半个文件）*/
bool app_store_export_progress(const char *path, const char *scope_dir, int total,
                               const char *cur_group, const char *cur_char);

#ifdef __cplusplus
}
#endif
#endif /* APP_STORE_H */
