#ifndef USER_CONFIG_H
#define USER_CONFIG_H

/*****************************************************************************
 * 板子选择 —— 改下面 APP_BOARD 这一行切换硬件
 *
 * 两块板的引脚几乎完全不同，而且大面积冲突（墨水屏的 6/8/9/10/11/12 正好是
 * AMOLED 板的音频总线和屏幕 QSPI）。所以必须编译期切，不能运行时判断：
 * app_power.cpp 里 board_power_bsp_t 是静态对象，构造函数在 app_main 之前
 * 就会把那几个脚配成输出。
 *****************************************************************************/
#define BOARD_EPAPER_1_54  1   /* Waveshare ESP32-S3-ePaper-1.54G  200x200 四色墨水屏 */
#define BOARD_AMOLED_1_8   2   /* Waveshare ESP32-S3-Touch-AMOLED-1.8  368x448 AMOLED */

#define APP_BOARD  BOARD_AMOLED_1_8

/*****************************************************************************
 * 各板差异
 *****************************************************************************/
#if APP_BOARD == BOARD_EPAPER_1_54

#define CODEC_BOARD_NAME    "S3_ePaper_1_54"

/*I2C*/
#define ESP32_I2C_SDA_PIN   GPIO_NUM_47
#define ESP32_I2C_SCL_PIN   GPIO_NUM_48

/*SD 卡（1 线 SDMMC）*/
#define SDMMC_CLK_PIN       39
#define SDMMC_CMD_PIN       41
#define SDMMC_D0_PIN        40

/*按键*/
#define BOOT_BUTTON_PIN     GPIO_NUM_0
#define PWR_BUTTON_PIN      GPIO_NUM_18

/*板级能力*/
/* ES8311 的 PGA 增益。驱动把 >=42 的都写成最大档 42 dB；它自己初始化的默认值
 * 是 REG16=0x24（约 24 dB），厂商 BSP 就用这个默认值、从不调 set_in_gain。
 * 这块板的麦配 45（=42 dB 满档）是原作者调出来的，别动。*/
#define MIC_GAIN_DB         45.0f
#define BOARD_HAS_EPAPER      1   /* 有墨水屏，display_task 走 epaper_port */
#define BOARD_HAS_AMOLED      0   /* 没有 AMOLED */
#define BOARD_HAS_GPIO_POWER  1   /* VBAT 自锁 / 屏电源 / 音频电源 是三个普通 GPIO */
#define BOARD_HAS_PWR_BUTTON  1   /* PWR 键接在 GPIO 上，multi_button 能读 */
#define BOARD_HAS_BAT_ADC     1   /* 电池电压走 ADC */
#define BOARD_HAS_TOUCH       0   /* 没有触摸屏 */

#elif APP_BOARD == BOARD_AMOLED_1_8

#define CODEC_BOARD_NAME    "S3_AMOLED_1_8"

/*I2C —— ES8311 / CST816 触摸 / QMI8658 / PCF85063 / AXP2101 / TCA9554 共用这一条*/
#define ESP32_I2C_SDA_PIN   GPIO_NUM_15
#define ESP32_I2C_SCL_PIN   GPIO_NUM_14

/*SD 卡（1 线 SDMMC）*/
#define SDMMC_CLK_PIN       2
#define SDMMC_CMD_PIN       1
#define SDMMC_D0_PIN        3

/*按键 —— PWR 键接的是 AXP2101 的 PKEY 脚，靠 I2C 中断读，不是 GPIO，所以这里没有*/
#define BOOT_BUTTON_PIN     GPIO_NUM_0

/*板级能力*/
/* 贴片麦比墨水屏板那颗灵敏，满档 42 dB 会把底噪顶到 -28 dB，
 * VAD 于是一直判「有人声」，WakeNet 在这层噪声里匹配不上唤醒词。
 * 先按厂商 BSP 的默认档位来。*/
#define MIC_GAIN_DB         24.0f
#define BOARD_HAS_EPAPER      0   /* 没有墨水屏 */
#define BOARD_HAS_AMOLED      1   /* CO5300 QSPI 彩屏，display_task 走 amoled_port */
#define BOARD_HAS_GPIO_POWER  0   /* 电源归 AXP2101 管，走 I2C */
#define BOARD_HAS_PWR_BUTTON  0   /* PWR 键在 AXP2101 上 */
#define BOARD_HAS_BAT_ADC     0   /* 电量也从 AXP2101 读；GPIO4 在这块板上是屏的 DATA0 */
#define BOARD_HAS_TOUCH       1   /* CST816S 电容触摸(0x15)，这块板没有 PWR 键，靠它补上 */

/*屏 CO5300 QSPI
 *  CS=12 PCLK=11 D0=4 D1=5 D2=6 D3=7，走 SPI2_HOST，40 MHz x 4 线
 *  屏没有复位 GPIO（厂商 BSP 里 BSP_LCD_RST = GPIO_NUM_NC），也没有背光脚，
 *  亮度是发面板命令 0x51。但复位并没有消失 —— 它和屏的供电一起挂在
 *  TCAL9534(0x20) 这个 I2C IO 扩展器上，厂商 BSP v2.0.3 里缺了这段，
 *  不补就是彻底黑屏且一条错误日志都没有（见 amoled_port.c 的 expander_panel_power）。
 *  面板 368x448 RGB565；触摸 CST816S(0x15)，INT=21，复位也在扩展器上 */
#define AMOLED_LCD_CS_PIN    GPIO_NUM_12
#define AMOLED_LCD_PCLK_PIN  GPIO_NUM_11
#define AMOLED_LCD_D0_PIN    GPIO_NUM_4
#define AMOLED_LCD_D1_PIN    GPIO_NUM_5
#define AMOLED_LCD_D2_PIN    GPIO_NUM_6
#define AMOLED_LCD_D3_PIN    GPIO_NUM_7
#define AMOLED_TOUCH_INT_PIN GPIO_NUM_21
#define AMOLED_IO_EXPANDER_ADDR 0x20
#define AMOLED_WIDTH         368
#define AMOLED_HEIGHT        448

#else
#error "APP_BOARD 没设对：只能是 BOARD_EPAPER_1_54 或 BOARD_AMOLED_1_8"
#endif

/*****************************************************************************
 * 墨水屏引脚 —— 只有 epaper_port 组件用。
 * AMOLED 板上这些脚另有用途（8/9/10 是 I2S，11/12 是屏 QSPI，6 是屏 DATA2），
 * 所以那块板上所有 epaper_port_* 调用都被 BOARD_HAS_EPAPER 挡掉了，
 * 这些宏留着只是让组件能编过。别在 AMOLED 板上调用 epaper_port_init()。
 *****************************************************************************/
#define EPD_SPI_NUM        SPI2_HOST
#define ESP32_I2C_DEV_NUM  I2C_NUM_0

#define EPD_WIDTH  200
#define EPD_HEIGHT 200
#define LVGL_SPIRAM_BUFF_LEN (EPD_WIDTH * EPD_HEIGHT * 2)

#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8

/*DEV POWER init —— 同上，只有 BOARD_HAS_GPIO_POWER 的板子会真去驱动*/
#define EPD_PWR_PIN     GPIO_NUM_6
#define Audio_PWR_PIN   GPIO_NUM_42
#define VBAT_PWR_PIN    GPIO_NUM_17

/*Low-power wake-up*/
#define ext_wakeup_pin_1 GPIO_NUM_0

/*I2C 从设备地址 —— i2c_bsp 需要（原在音频例程的 user_config.h 中）*/
#define I2C_RTC_DEV_Address    0x51
#define I2C_SHTC3_DEV_Address  0x70

#endif // !USER_CONFIG_H
