/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 唤醒词: 你好小智 (wn10_nihaoxiaozhi)
 * 命令词: MultiNet7 中文。固定命令 + 查字（「如果的如」，不分组，清单里的字都能查）
 *
 * 本模块只负责"听"，不持有任何学习状态：识别结果通过回调交给应用层，
 * 清单解析也由应用层完成（语音没启动时按键导航照样可用）。
 *****************************************************************************/
#ifndef APP_SR_H
#define APP_SR_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 固定命令编号 */
typedef enum {
    SR_CMD_NEXT = 0,      /* 下一个 */
    SR_CMD_PREV,          /* 上一个 */
    SR_CMD_REPEAT,        /* 再读一遍 */
    SR_CMD_WORDS,         /* 读词组 */
    SR_CMD_MASTERED,      /* 我会了 */
    SR_CMD_REVIEW,        /* 复习 */
    SR_CMD_FORGOT,        /* 忘了 */
    SR_CMD_BYE,           /* 再见 —— 熄屏待机，不断电 */
    SR_CMD_FIXED_COUNT,
} sr_fixed_cmd_t;

/* 查字: SR_CMD_CHAR_BASE + 字 id（「如果的如」→ 如的 id）*/
#define SR_CMD_CHAR_BASE     100

/* 回调里的非命令事件 */
#define SR_EVT_WAKE          (-1)   /* 听到唤醒词 */
#define SR_EVT_TIMEOUT       (-2)   /* 唤醒后一直没听到命令 */
#define SR_EVT_NOT_FOUND     (-3)   /* 听到「…的X」，但对不上是哪个字 */

typedef struct {
    bool  wake_high;      /* true: WakeNet 用 DET_MODE_95（更灵敏，误唤醒也更多）*/
    float mn_threshold;   /* MultiNet 判定阈值，<= 0 用模型默认值 */
    int   listen_ms;      /* 唤醒后等命令的时间 */
    int   follow_up_ms;   /* 命令执行后继续听的时间，0 = 每次都要重新唤醒 */
} sr_config_t;

/* 识别回调：cmd_id 为上面的命令编号或 SR_EVT_*。
 * 在识别任务里调用，实现方只能投递消息，不能阻塞。*/
typedef void (*sr_cmd_cb_t)(int cmd_id);

/* 启动语音识别（加载模型，创建 feed / detect 两个任务），约需数秒。
 * 启动后先只有固定命令，登记了查字表才能查字。*/
bool app_sr_start(sr_cmd_cb_t cb, const sr_config_t *cfg);

/* 登记查字表: 字 ids[i] 的说法是 phrases[i]（"ru guo de ru,bi ru de ru"）。
 * 只认「…de X」形式的说法（按最后一个音节 X 分份，见 app_sr.cpp「对原始文本」）。
 * 数组和字符串之后要一直有效，内部只存指针。
 * 返回说法条数，失败返回 -1。开机调用一次，可在识别运行中调用（内部加锁）。*/
int  app_sr_set_lookup(const int *ids, const char *const *phrases, int n);

/* 调试: 把一句拼音（"ru guo de guo"）当成听到的原始文本去查字，日志里列出最像的几个字。
 * 返回 SR_CMD_CHAR_BASE + 字 id，对不上返回 SR_EVT_NOT_FOUND。*/
int  app_sr_lookup_text(const char *text);

/* 播放期间屏蔽拾音：板载喇叭离麦克风很近，不屏蔽会自激误触发。*/
void app_sr_set_muted(bool muted);

/* 按键代替唤醒词：下一帧就进监听窗口，和听到「你好小智」走同一条路（关掉 WakeNet、
 * 照样回调 SR_EVT_WAKE）。已经在听的时候再调一次只是把窗口续满，不会再响一次提示音。
 * 可在任何任务里调用（只置一个标志）；语音没启动时调用无害，没人读这个标志。*/
void app_sr_wake(void);

#ifdef __cplusplus
}
#endif
#endif /* APP_SR_H */
