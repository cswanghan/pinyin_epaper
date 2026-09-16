/*****************************************************************************
 * 音频播放 —— 独立任务，与刷屏并行
 *
 * 三类请求共用一个队列、按顺序播放:
 *   WAV     SD 卡上的朗读（字音 / 词组）
 *   PROMPT  语音提示 <SD>/pinyin/sys/<key>.wav，文件不存在就用固件合成的提示音代替
 *   TONE    固件合成的提示音（叮 / 嘟嘟 …），不依赖 SD 卡
 *
 * 播放期间自动屏蔽语音识别（app_sr_set_muted），队列放空后才恢复拾音。
 *****************************************************************************/
#ifndef APP_AUDIO_H
#define APP_AUDIO_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TONE_WAKE = 0,    /* 叮 —— 唤醒，可以说命令了 */
    TONE_BUSY,        /* 嘟嘟 —— 正在刷屏，等一下 */
    TONE_TIMEOUT,     /* 下行两音 —— 没听到命令 */
    TONE_OK,          /* 上行两音 —— 标记"我会了" */
    TONE_FORGOT,      /* 下行两音 —— 取消标记 */
    TONE_DONE,        /* 上行琶音 —— 这一组都会了 */
    TONE_GROUP,       /* 两声短音 —— 切换了学习组 */
    TONE_BYE,         /* 下行琶音 —— 关机 */
    TONE_COUNT,
} tone_t;

void app_audio_init(const char *sd_root);       /* 创建播放任务 */
void app_audio_set_volume(int vol);             /* 0-100 */

void app_audio_play_file(const char *path);     /* 排队播放 WAV */
void app_audio_play_char(int id, char kind);    /* 'c' 字音 / 'w' 词组 */
void app_audio_prompt(const char *key, tone_t fallback);
void app_audio_tone(tone_t tone);
void app_audio_gap(int ms);                     /* 排一段静默（念笔顺时留给孩子跟着写）*/

/* 丢弃排队中的请求并打断正在播放的朗读（切字时用，避免旧字的声音拖尾）*/
void app_audio_stop(void);
bool app_audio_idle(void);                      /* 没有在播、也没有排队 */

#ifdef __cplusplus
}
#endif
#endif /* APP_AUDIO_H */
