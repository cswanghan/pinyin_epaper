/*****************************************************************************
 * CST816S 电容触摸（I2C 0x15，INT=GPIO21）
 *
 * 这块板的 PWR 键不是 GPIO，挂在 AXP2101 上，multi_button 读不到 ——
 * 触摸就是用来把按键缺口补上的：
 *   轻点任意位置  → 唤醒语音（等同墨水屏板上单击 BOOT）
 *   向左滑        → 下一个字
 *   向右滑        → 上一个字
 *
 * 手势不读芯片的 GestureID 寄存器 —— CST816 家族几个型号的手势编码对不上，
 * 而且滑动阈值不可调。这里只取原始坐标，按下/抬起自己算位移，行为完全可控。
 *
 * 触摸的复位脚也在 TCAL9534 上，amoled_port 的上电时序已经把它放开了，
 * 所以 touch_bsp_init() 必须排在 amoled_port_init() 之后。
 *****************************************************************************/
#ifndef TOUCH_BSP_H
#define TOUCH_BSP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_TAP = 0,
    TOUCH_SWIPE_LEFT,      /* 手指从右往左划 */
    TOUCH_SWIPE_RIGHT,
} touch_evt_t;

/* 回调在触摸任务里跑，别在里面干耗时的事，post 一条命令就回来。*/
typedef void (*touch_cb_t)(touch_evt_t evt);

/* 探测芯片、配好 INT、起触摸任务。没插触摸或读不到芯片 ID 就返回 false。*/
bool touch_bsp_init(touch_cb_t cb);

#ifdef __cplusplus
}
#endif
#endif
