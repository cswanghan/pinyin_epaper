/*****************************************************************************
 * 调试控制台 —— 通过 USB 串口（idf.py monitor）敲命令，不用对着设备喊
 *
 *   n / p        下一个 / 上一个        r / w     重播字音 / 读词组
 *   m / f        我会了 / 忘了          v         复习
 *   gn / gp      下一组 / 上一组        g N       第 N 组
 *   c N          跳到清单第 N 个字（从 0 开始）
 *   l            列出本组的字和跳字说法（跳字命令只对本组有效）
 *   find X       在所有清单里找 X（汉字或拼音），看它在第几组
 *   wake/timeout 模拟唤醒 / 超时提示音  s         状态
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

typedef void (*console_post_t)(int cmd);
void app_console_start(console_post_t post);

#ifdef __cplusplus
}
#endif
#endif /* APP_CONSOLE_H */
