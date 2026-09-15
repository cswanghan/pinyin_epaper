/*****************************************************************************
 * 离线语音识别 —— WakeNet 唤醒 + MultiNet 中文命令词
 *
 * 唤醒词: 你好小智 (wn10_nihaoxiaozhi)
 * 命令词: MultiNet7 中文，运行时由应用层按当前学习清单注册
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
    SR_CMD_GROUP_NEXT,    /* 下一组 */
    SR_CMD_GROUP_PREV,    /* 上一组 */
    SR_CMD_FIXED_COUNT,
} sr_fixed_cmd_t;

/* 跳字命令: SR_CMD_CHAR_BASE + 字在当前清单中的下标（「如果的如」）*/
#define SR_CMD_CHAR_BASE     100
#define SR_MAX_SCOPE_CHARS   200
/* 选组命令: SR_CMD_GROUP_BASE + N，N 从 1 开始（「第三组」）*/
#define SR_CMD_GROUP_BASE    300
#define SR_MAX_GROUPS        99

/* 回调里的非命令事件 */
#define SR_EVT_WAKE          (-1)   /* 听到唤醒词 */
#define SR_EVT_TIMEOUT       (-2)   /* 唤醒后一直没听到命令 */

typedef struct {
    bool  wake_high;      /* true: WakeNet 用 DET_MODE_95（更灵敏，误唤醒也更多）*/
    float mn_threshold;   /* MultiNet 判定阈值，<= 0 用模型默认值 */
    int   listen_ms;      /* 唤醒后等命令的时间 */
    int   follow_up_ms;   /* 命令执行后继续听的时间，0 = 每次都要重新唤醒 */
} sr_config_t;

/* 识别回调：cmd_id 为上面的命令编号或 SR_EVT_*。
 * 在识别任务里调用，实现方只能投递消息，不能阻塞。*/
typedef void (*sr_cmd_cb_t)(int cmd_id);

/* 启动语音识别（加载模型，创建 feed / detect 两个任务），约需数秒。*/
bool app_sr_start(sr_cmd_cb_t cb, const sr_config_t *cfg);

/* 重新注册命令词：固定命令 + 「第1组」…「第group_count组」+ 当前清单的跳字说法。
 * phrases[i] 是清单第 i 个字的说法（"ru guo de ru,ru guo"），可为 NULL。
 * 超过 400 条上限时按 固定 > 选组 > 跳字 的优先级截断。
 * 返回被模型拒绝的条数，失败返回 -1。可在识别运行中调用（内部加锁）。*/
int  app_sr_set_scope(const char *const *phrases, int n, int group_count);

/* 播放期间屏蔽拾音：板载喇叭离麦克风很近，不屏蔽会自激误触发。*/
void app_sr_set_muted(bool muted);

#ifdef __cplusplus
}
#endif
#endif /* APP_SR_H */
