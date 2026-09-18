/*****************************************************************************
 * AMOLED 面板（CO5300, 368x448, QSPI）
 *
 * 和 epaper_port 同一个位置：只负责把像素送上屏，不掺业务。
 * 跟墨水屏最大的不同是刷屏只要十几毫秒而不是 20 秒 —— 上层那套
 * 「插队换字」「刷屏窗口自校准」在这块板上都不再是必需，但接口保持简单，
 * 让 user_app 能用同一套流程。
 *
 * 像素格式 RGB565，大端（高字节在前）—— SD 卡上的图就按这个字节序存，
 * 设备端不做转换。
 *****************************************************************************/
#ifndef AMOLED_PORT_H
#define AMOLED_PORT_H

#include <stdint.h>
#include <stdbool.h>

#define AMOLED_W  368
#define AMOLED_H  448
#define AMOLED_FB_BYTES  (AMOLED_W * AMOLED_H * 2)
/* 一次 SPI 事务最多 32 KB（S3 的 DMA 长度寄存器只有 18 位），32 行 = 23552 字节，正好整除 448 */
#define AMOLED_BAND_ROWS  32
#define AMOLED_BAND_BYTES (AMOLED_W * AMOLED_BAND_ROWS * 2)

#ifdef __cplusplus
extern "C" {
#endif

/* 建 QSPI 总线、跑面板初始化命令表、开显示。失败返回 false。*/
bool amoled_port_init(void);

/* 0~100。这块屏没有背光脚，亮度是给面板发 0x51 命令。*/
void amoled_port_brightness(int percent);

/* 整屏刷新。fb 要 AMOLED_FB_BYTES 字节，RGB565 大端。*/
void amoled_port_draw(const uint16_t *fb);

/* 只送屏上的一条横带：fb 仍是整屏缓冲，从第 y0 行起送 rows 行。
 * 笔顺动画每帧只改字框里的几行，整屏送要 29 ms，按行送 1~2 ms。
 * 面板的行地址是按整行给的，所以横向不做裁剪 —— 反正一行才 736 字节。*/
void amoled_port_draw_rows(const uint16_t *fb, int y0, int rows);

/* 整屏填单色（RGB565 主机字节序，函数内部转成面板要的字节序）。*/
void amoled_port_fill(uint16_t color);

/* 点亮校验图：四色条带 + 左上角黄块 + 右下角青块 + 贴着四边的 3px 黑框。
 * 红显成红的 → RGB565 字节序对；黄块在左上 → 原点方向对；
 * 黑框四条边都完整 → x_gap 对、368x448 整屏都够得着。*/
void amoled_port_test_pattern(void);


/* 熄屏进休眠。*/
void amoled_port_sleep(void);

/* RGB888 → RGB565，大端。给设备端自己画东西用（测试图、提示条）。*/
static inline uint16_t amoled_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));       /* 转大端 */
}

#ifdef __cplusplus
}
#endif
#endif
