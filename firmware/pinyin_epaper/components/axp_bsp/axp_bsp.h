/*****************************************************************************
 * AXP2101 电源管理芯片（I2C 0x34）—— 只有 AMOLED 板有
 *
 * 这块板上它管着一切：电池充放电、各路 LDO/DCDC、还有 PWR 键。
 * PWR 键没有引到 GPIO 上，是接在 AXP2101 的 PKEY 脚，按下由芯片置中断位，
 * 主控只能通过 I2C 读出来 —— 所以 multi_button 看不见这个键。
 *
 * 【寄存器含义是探出来的，不是抄手册的】
 * 厂商 BSP（esp-bsp 的 waveshare/esp32_s3_touch_amoled_1_8）根本没碰 AXP2101，
 * 手册和 XPowersLib 在这台机器上又下不下来（公司网挡了 github）。所以做法是
 * AXP_PROBE=1 把整片寄存器扫一遍，让人按 PWR 键，看谁变了。2026-09-20 实机
 * 三次按键的日志（短按 190 ms / 按住 1150 ms / 短按 421 ms）给出：
 *
 *   0x49 bit1 = 按下     每次按键先跳这一位
 *   0x49 bit0 = 松手     紧跟着和 bit3 一起跳
 *   0x49 bit3 = 芯片判定的「短按」
 *   0x49 bit2 = 芯片判定的「长按」，阈值在 0x27 里配着 1.5 秒，没碰到过
 *
 * 长按短按不看芯片那两位，自己掐 bit1 到 bit0 的毫秒数（见 KEY_LONG_MS）——
 * 阈值归固件定，也省得再上板试一轮 bit2 到底是不是长按。
 *
 * 0x48..0x4A 这三个中断状态寄存器是「写 1 清零」的锁存器（实测写 0xFF 三个
 * 全清成 00）。读完必须把看见的那几位写回去，否则同一位只会报一次 0→1，
 * 第二下、第三下全丢 —— 上一轮探针就栽在这儿。
 *
 * 其余探明的位置（开机底档 + 交叉验证）:
 *   0x03 芯片型号，读出 0x4A    0x10 通用配置，底档 0x34，bit0 = 软关机
 *   0x34/0x35 电池电压 mV（高字节在前，实测 4102）   0xA4 电量百分比
 *   0x40..0x42 中断使能，底档 FF FC 5F（默认全开）
 *
 * 【安全边界】只写中断使能/中断状态/软关机这三处。绝不碰 0x80..0x9F ——
 * 那一片是各路 DCDC/LDO 的使能和电压，写错能把 3.3V 调到 3.6V，
 * 是真会烧主控和屏的。读随便读，写就这三处。
 *****************************************************************************/
#ifndef AXP_BSP_H
#define AXP_BSP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 探针模式：只读扫描寄存器并打印变化。寄存器含义已经探明（见上），平时保持 0；
 * 换板子或者要找新的位（电量告警、充电状态之类）再临时打开。*/
#define AXP_PROBE  0

/* 探芯片。没有 AXP2101 返回 false，后面几个函数就都别调了。*/
bool axp_bsp_init(void);

/* 把 0x00..0x7F 和 0xA0..0xAF 打到日志里（16 个一行）。0x80..0x9F 故意不打。*/
void axp_bsp_dump(const char *why);

typedef enum {
    AXP_KEY_SHORT = 0,   /* 短按一下 */
    AXP_KEY_LONG,        /* 按住超过 KEY_LONG_MS，手还没松就报了 */
} axp_key_t;

/* 回调在 PKEY 轮询任务里跑，别在里面干耗时的事，post 一条命令就回来。*/
typedef void (*axp_key_cb_t)(axp_key_t key);

/* 起 PKEY 轮询任务（30 ms 一次，只读 0x49 一个字节）。
 * AXP 的中断脚在这块板上没接出来，只能轮询。*/
void axp_bsp_key_start(axp_key_cb_t cb);

/* 软关机：写 0x10 bit0，芯片切断所有输出，不返回。
 * 2026-09-20 实测：插着 USB 也照断不误，主控当场没电、串口从系统里消失。
 *
 * 【怎么开回来，还没定论】关掉之后长按 PWR 键试了两次都没反应，最后是重新插
 * USB 才活过来 —— 0x20（开机原因）读出 0x04 = VBUS 插入，不是 0x01 = 按键。
 * 但那两次身上还插着 USB，AXP 关机后 VBUS 一直在，可能是它压根不认这个键，
 * 也可能是带电状态下的另一套逻辑。要断言只能脱开 USB、纯电池试一次。
 * 在那之前别把「长按关机」当成能用的功能推给孩子：关得掉、开不回来更糟。*/
void axp_bsp_shutdown(void);

int axp_bsp_vbat_mv(void);    /* 电池电压 mV，读不到或明显不合理返回 -1 */
int axp_bsp_percent(void);    /* 电量 0..100，读不到返回 -1 */

#if AXP_PROBE
/* 只读监视任务：反复扫状态区，哪个寄存器变了就打一行。*/
void axp_bsp_probe_start(void);
#endif

#ifdef __cplusplus
}
#endif
#endif /* AXP_BSP_H */
