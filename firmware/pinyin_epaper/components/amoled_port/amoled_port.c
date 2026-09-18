/*****************************************************************************
 * AMOLED 面板（CO5300, 368x448, QSPI）—— 见 amoled_port.h
 *****************************************************************************/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"

#include "amoled_port.h"
#include "user_config.h"
#include "i2c_bsp.h"

/* 墨水屏那块板上这个组件照样参与编译（组件列表不认编译期开关），但那块板没有
 * AMOLED_* 引脚宏，也不该建 QSPI 总线 —— 整个实现在这里切掉，只留空壳。
 * 和 epaper_port 在 AMOLED 板上的处境正好对称。*/
#if !BOARD_HAS_AMOLED

bool amoled_port_init(void)        { return false; }
void amoled_port_brightness(int p) { (void)p; }
void amoled_port_draw(const uint16_t *fb) { (void)fb; }
void amoled_port_draw_rows(const uint16_t *fb, int y0, int rows) { (void)fb; (void)y0; (void)rows; }
void amoled_port_fill(uint16_t c)  { (void)c; }
void amoled_port_test_pattern(void) { }
void amoled_port_sleep(void)       { }

#else

#define TAG "AMOLED"

#define LCD_SPI_HOST   SPI2_HOST
#define AMOLED_X_GAP   0x10
#define LCD_BPP        16

static esp_lcd_panel_handle_t    s_panel = NULL;
static esp_lcd_panel_io_handle_t s_io    = NULL;

/* 面板初始化命令表：抄自厂商 BSP（esp32_s3_touch_amoled_1_8.c）。
 * 这张表是这块屏特有的，CO5300 驱动自带的默认表点不亮它。
 *   0x3A=0x55  16 bpp
 *   0x53/0x51  打开亮度控制并拉满
 *   0x2A/0x2B  列 0..0x16F(=367)、行 0..0x1BF(=447)
 *   0x11       退出睡眠（等 100 ms）
 *   0x29       开显示 */
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

/* 屏的供电和复位挂在 TCAL9534(0x20) 扩展器上，不是 GPIO。
 * 厂商 BSP v2.0.3（我们手里那份）压根没这段，照着它抄就是屏永远不通电 —— 黑屏的根因。
 * 出厂固件的时序是从它自己的 ESP_ERROR_CHECK 断言字符串里还原的：
 *     write_reg(0x03, 0x78)                                     P0/P1/P2/P7 设成输出
 *     set_output(sd_cs)
 *     set_output(dsi_power | sd_cs)                             屏上电
 *     set_output(dsi_power | sd_cs | touch_reset)               放开触摸复位
 *     set_output(dsi_power | sd_cs | touch_reset | lcd_reset)   放开屏复位，= 0x87
 * 四个信号确实占 P0/P1/P2/P7（和我们实测读到的 方向=0x78 输出=0x87 对得上），
 * 但字符串没说哪个信号是哪个脚。真正要紧的只有一条：dsi_power 得比 lcd_reset 先高。
 * 所以按位序从低到高释放、从高到低释放各做一版，必有一版满足这个先后关系。*/
#define EXP_ADDR     0x20
#define EXP_REG_OUT  0x01
#define EXP_REG_CFG  0x03
#define EXP_CFG      0x78          /* P0 P1 P2 P7 输出，P3..P6 保持输入 */
#define EXP_OUT_ALL  0x87          /* 四个信号全释放，出厂固件的最终值 */

static void expander_panel_power(void)
{
    /* 按位序从低到高逐个释放，终值 0x87 = 出厂固件的最终状态 */
    static const uint8_t ramp[4] = {0x01, 0x03, 0x07, 0x87};

    i2c_poke(EXP_ADDR, EXP_REG_CFG, EXP_CFG);
    i2c_poke(EXP_ADDR, EXP_REG_OUT, 0x00);        /* 全拉低：断电 + 复位按住 */
    vTaskDelay(pdMS_TO_TICKS(120));
    for (int i = 0; i < 4; i++) {
        i2c_poke(EXP_ADDR, EXP_REG_OUT, ramp[i]);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    vTaskDelay(pdMS_TO_TICKS(150));               /* 复位放开后等屏内部起来 */

    uint8_t back = 0;
    i2c_peek(EXP_ADDR, EXP_REG_OUT, &back, 1);
    ESP_LOGI(TAG, "TCAL9534 上电时序完成，回读输出=0x%02X", back);
}


bool amoled_port_init(void)
{
    if (s_panel) return true;

    expander_panel_power();

    spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        AMOLED_LCD_PCLK_PIN, AMOLED_LCD_D0_PIN, AMOLED_LCD_D1_PIN,
        AMOLED_LCD_D2_PIN, AMOLED_LCD_D3_PIN, AMOLED_BAND_BYTES);
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QSPI 总线建不起来: %s", esp_err_to_name(err));
        return false;
    }

    esp_lcd_panel_io_spi_config_t io = CO5300_PANEL_IO_QSPI_CONFIG(AMOLED_LCD_CS_PIN, NULL, NULL);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io 建不起来: %s", esp_err_to_name(err));
        return false;
    }

    co5300_vendor_config_t vendor = {
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = GPIO_NUM_NC,      /* 这块板没有复位脚，靠命令表软启 */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BPP,
        .vendor_config  = &vendor,
    };
    err = esp_lcd_new_panel_co5300(s_io, &dev, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CO5300 面板建不起来: %s", esp_err_to_name(err));
        s_panel = NULL;
        return false;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* X 偏移 16 —— 厂商 BSP v2.0.3 的变更说明写得很直白：探到 CST816S 触摸就给
     * CO5300 加 0x10 的 X offset（FT5x06 那版不加）。我们这块 I2C 上有 0x15，
     * 就是 CST816S 版。面板 RAM 的可见列是 16..383，从 0 开始写会落在窗口外。*/
    esp_lcd_panel_set_gap(s_panel, AMOLED_X_GAP, 0);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "面板就绪 %dx%d，整屏 %d KB", AMOLED_W, AMOLED_H, AMOLED_FB_BYTES / 1024);
    return true;
}

void amoled_port_brightness(int percent)
{
    if (!s_io) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    /* QSPI 下命令要拼成 32 位：0x02<<24 | cmd<<8（厂商 BSP 就是这么发的）*/
    uint8_t v = (uint8_t)(percent * 255 / 100);
    esp_lcd_panel_io_tx_param(s_io, (0x02u << 24) | (0x51u << 8), &v, 1);
}

/* 分条把整屏送出去。
 * 不能一次 draw_bitmap 整屏 —— PSRAM 的地址（0x3C......）不在 SOC_DMA_LOW..HIGH 里，
 * SPI 驱动认定它不是 DMA 内存，会给每个分片临时申请一块内部 DMA 内存做中转。
 * esp_lcd 按 32 KB 切片、队列深 10，322 KB 整屏要 320 KB 内部 RAM，板上只有 ~212 KB，
 * 于是 spi_device_queue_trans 返回 NO_MEM，一个像素都没送出去 —— 这就是之前黑屏的原因。
 * 每条 32 行 = 23552 字节 < 32 KB，单片成行；而 draw_bitmap 发命令前会先把在飞的事务收干净，
 * 所以中转内存同一时刻只占一块。*/
static bool panel_push(const uint16_t *src)
{
    for (int y = 0; y < AMOLED_H; y += AMOLED_BAND_ROWS) {
        int rows = AMOLED_H - y;
        if (rows > AMOLED_BAND_ROWS) rows = AMOLED_BAND_ROWS;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, AMOLED_W, y + rows,
                                                  src + (size_t)y * AMOLED_W);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "送屏失败 y=%d: %s", y, esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

void amoled_port_draw(const uint16_t *fb)
{
    if (!s_panel || !fb) return;
    panel_push(fb);
}

void amoled_port_draw_rows(const uint16_t *fb, int y0, int rows)
{
    if (!s_panel || !fb) return;
    if (y0 < 0) { rows += y0; y0 = 0; }
    if (y0 >= AMOLED_H || rows <= 0) return;
    if (y0 + rows > AMOLED_H) rows = AMOLED_H - y0;

    /* 和 panel_push 一样得分条 —— 单次事务上限 32 KB，源在 PSRAM 不能直接 DMA */
    for (int y = y0; y < y0 + rows; y += AMOLED_BAND_ROWS) {
        int n = y0 + rows - y;
        if (n > AMOLED_BAND_ROWS) n = AMOLED_BAND_ROWS;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, AMOLED_W, y + n,
                                                  fb + (size_t)y * AMOLED_W);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "送行失败 y=%d n=%d: %s", y, n, esp_err_to_name(err));
            return;
        }
    }
}

/* 整屏缓冲，放 PSRAM。
 * 本来是借一行内存逐行画的，那是个错 —— esp_lcd 送色块走的是 spi_device_queue_trans，
 * 排进队列就返回，队列深 10；下一行已经把同一块内存改掉了，前面几行发出去的是花的。
 * 322 KB 在 8 MB PSRAM 里不算什么，一次发完还省 447 次事务开销。*/
static uint16_t *frame_buf(void)
{
    static uint16_t *fb = NULL;
    if (!fb) {
        fb = (uint16_t *)heap_caps_malloc(AMOLED_FB_BYTES, MALLOC_CAP_SPIRAM);
        if (!fb) fb = (uint16_t *)heap_caps_malloc(AMOLED_FB_BYTES, MALLOC_CAP_8BIT);
        if (!fb) ESP_LOGE(TAG, "整屏缓冲 %d 字节申请不到", AMOLED_FB_BYTES);
    }
    return fb;
}

void amoled_port_fill(uint16_t color)
{
    if (!s_panel) return;
    uint16_t *fb = frame_buf();
    if (!fb) return;
    /* color 已经是 amoled_rgb() 转好的大端，这儿不能再翻一次 ——
     * 原来多翻了一遍，红的会画成蓝的 */
    for (int i = 0; i < AMOLED_W * AMOLED_H; i++) fb[i] = color;
    panel_push(fb);
}

/* 点亮验收图。三件事一次看完：
 *   1. QSPI 链路通不通 —— 屏上有东西就是通了
 *   2. RGB565 字节序对不对 —— 上红中绿下蓝。字节序反了红会变成暗蓝，一眼能看出来
 *   3. 原点和方向对不对 —— 左上角有个黄方块，底边一条黑边 */
void amoled_port_test_pattern(void)
{
    if (!s_panel) return;
    uint16_t *fb = frame_buf();
    if (!fb) return;

    const uint16_t band[4] = {
        amoled_rgb(255, 0, 0), amoled_rgb(0, 255, 0),
        amoled_rgb(0, 0, 255), amoled_rgb(255, 255, 255),
    };
    const uint16_t yellow = amoled_rgb(255, 220, 0);
    const uint16_t cyan   = amoled_rgb(0, 255, 255);
    const uint16_t black  = amoled_rgb(0, 0, 0);

    for (int y = 0; y < AMOLED_H; y++) {
        uint16_t *row = fb + (size_t)y * AMOLED_W;
        uint16_t c = band[y * 4 / AMOLED_H];
        for (int x = 0; x < AMOLED_W; x++) row[x] = c;
        /* 左上角黄块 = 原点标记；右下角青块 = 右下角确实在屏内 */
        if (y < 56) for (int x = 0; x < 56; x++) row[x] = yellow;
        if (y >= AMOLED_H - 56) for (int x = AMOLED_W - 56; x < AMOLED_W; x++) row[x] = cyan;
        /* 贴着四边的 3px 黑框：四条边都完整 = 尺寸和 X 偏移都对。
         * 要是 x_gap 给错了，左边或右边那条竖线会缺掉或挤到画面里。*/
        if (y < 3 || y >= AMOLED_H - 3) {
            for (int x = 0; x < AMOLED_W; x++) row[x] = black;
        } else {
            row[0] = row[1] = row[2] = black;
            row[AMOLED_W - 3] = row[AMOLED_W - 2] = row[AMOLED_W - 1] = black;
        }
    }
    panel_push(fb);
    ESP_LOGI(TAG, "校验图已送屏：四色条带 + 左上黄块 + 右下青块 + 贴边黑框");
}

void amoled_port_sleep(void)
{
    if (!s_panel) return;
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
}

#endif /* BOARD_HAS_AMOLED */
