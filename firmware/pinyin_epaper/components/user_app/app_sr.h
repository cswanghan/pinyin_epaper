/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 唤醒词: 你好小智 (wn10_nihaoxiaozhi)
 * 命令词: MultiNet7 中文，运行时从 SD 卡的学习清单动态加载
 *****************************************************************************/
#ifndef APP_SR_H
#define APP_SR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定命令的编号。字跳转命令从 SR_CMD_CHAR_BASE 开始，
 * 编号 = SR_CMD_CHAR_BASE + 该字在当前清单中的下标。*/
typedef enum {
    SR_CMD_NEXT = 0,      /* 下一个 */
    SR_CMD_PREV,          /* 上一个 */
    SR_CMD_REPEAT,        /* 再读一遍 */
    SR_CMD_WORDS,         /* 读词组 */
    SR_CMD_MASTERED,      /* 我会了 */
    SR_CMD_REVIEW,        /* 复习 */
    SR_CMD_FIXED_COUNT,
} sr_fixed_cmd_t;

#define SR_CMD_CHAR_BASE  100

/* 识别结果回调。cmd_id 为上面的固定编号，
 * 或 >= SR_CMD_CHAR_BASE 表示跳转到清单中第 (cmd_id - SR_CMD_CHAR_BASE) 个字。*/
typedef void (*sr_cmd_cb_t)(int cmd_id);

/* 启动语音识别（会创建 feed / detect 两个任务）*/
bool app_sr_start(sr_cmd_cb_t cb);

/* 加载学习清单并注册命令词。
 * scope_path: SD 卡上的清单文件，如 /sdcard/pinyin/scope/01_基础字1-50.txt
 * 返回清单中的字数，失败返回 -1。
 * 清单中第 i 个字对应命令编号 SR_CMD_CHAR_BASE + i，
 * 其 id 可用 app_sr_scope_char_id(i) 取得。*/
int  app_sr_load_scope(const char *scope_path);

/* 取清单中第 index 个字的生字 id（用于加载图片/音频）*/
int  app_sr_scope_char_id(int index);

/* 朗读期间应抑制识别，避免扬声器的声音被麦克风拾取造成误触发。
 * 板载喇叭和麦克风距离很近，这个几乎必然发生。*/
void app_sr_set_muted(bool muted);

#ifdef __cplusplus
}
#endif
#endif /* APP_SR_H */
