/*****************************************************************************
 * 调试控制台 —— 通过 USB 串口（idf.py monitor）敲命令，不用对着设备喊
 *
 *   n / p        下一个 / 上一个（到组尾接着下一组）  r / w  重播字音 / 读词组
 *   m / f        我会了 / 忘了          v         复习
 *   c X          查字: 汉字或字 id（和说「如果的如」一样，不分组）
 *   gn / gp / g N 下一组 / 上一组 / 第 N 组（调试用，语音不能切组）
 *   l            列出本组的字、字 id 和查字说法
 *   find X       在所有清单里找 X（汉字或拼音），看它在第几组
 *   k            按键唤醒（等于单击 BOOT，真的进监听窗口）
 *   wake/timeout 只放唤醒 / 超时提示音，不进监听  s         状态
 *   export       立即导出 progress.txt  off       关机
 *****************************************************************************/
#ifndef APP_CONSOLE_H
#define APP_CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

/* 除 SR_CMD_* / SR_EVT_* 之外，控制台和按键还会投递这些应用事件 */
#define APP_EVT_POWER_OFF   (-10)
#define APP_EVT_STATUS      (-11)
#define APP_EVT_EXPORT      (-12)
#define APP_EVT_LIST        (-13)
#define APP_EVT_WAKE_KEY    (-14)   /* 单击 BOOT: 手动唤醒，进监听窗口等孩子说话 */
#define APP_EVT_RESHOW      (-15)   /* 点米字格: 这个字重来一遍（笔顺动画 + 朗读）*/
#define APP_EVT_PWR_KEY     (-16)   /* 短按 PWR: 亮着就熄屏，熄着就点亮 */
#define APP_EVT_AXP_DUMP    (-17)   /* 控制台 axp: 打一遍 PMU 寄存器（只读）*/

/* 切组只有控制台能做（数值避开 SR_CMD_CHAR_BASE + 字 id 的范围）*/
#define APP_CMD_GROUP_NEXT  90
#define APP_CMD_GROUP_PREV  91
#define APP_CMD_GROUP_BASE  10000    /* g N → APP_CMD_GROUP_BASE + N，N 从 1 开始 */

typedef void (*console_post_t)(int cmd);
void app_console_start(console_post_t post);

#ifdef __cplusplus
}
#endif
#endif /* APP_CONSOLE_H */
