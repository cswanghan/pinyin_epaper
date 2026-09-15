/*****************************************************************************
 * 电源 —— VBAT 自锁、电池电压、关机
 *
 * 板子的电池供电靠 GPIO17 自锁: PWR 键按下时临时接通电源，
 * 固件必须把 GPIO17 拉高"接住"，拉低就断电（即关机）。
 * 插着 USB 时拉低 GPIO17 断不了电，改为深度睡眠，按 PWR 唤醒。
 *****************************************************************************/
#ifndef APP_POWER_H
#define APP_POWER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void app_power_init(void);            /* 自锁 + 打开屏幕/音频电源域 + 电池 ADC */
int  app_power_vbat_mv(void);         /* 电池电压（mV），读不到返回 -1 */
bool app_power_usb_connected(void);   /* 连着电脑（USB 主机在枚举）*/
bool app_power_key_down(void);        /* PWR 键此刻是否按着 */
void app_power_off(void);             /* 不返回 */

#ifdef __cplusplus
}
#endif
#endif /* APP_POWER_H */
