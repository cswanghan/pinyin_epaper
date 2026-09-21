/*****************************************************************************
 * 电源实现
 *
 * 墨水屏板（BOARD_HAS_GPIO_POWER=1）的引脚（见 main/user_config.h）:
 *   GPIO17 VBAT 自锁（高 = 保持供电）   GPIO6 屏幕电源（低 = 开）
 *   GPIO42 音频电源（低 = 开）          GPIO18 PWR 键（低 = 按下）
 *   GPIO4  电池电压 ADC（板上 1:1 分压，实际电压 = 读数 × 2）
 *
 * AMOLED 板上这套全都不成立：电源归 AXP2101 管（I2C），PWR 键接在 AXP2101 的
 * PKEY 脚上，GPIO4/GPIO6 是屏幕 QSPI 的数据线 —— 碰一下就把屏总线拉坏。
 * 所以整块按 BOARD_HAS_GPIO_POWER 编译期切掉，包括那个静态对象：
 * 它的构造函数在 app_main 之前就跑，运行时判断根本来不及。
 *****************************************************************************/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"

#include "app_power.h"
#include "user_config.h"
#if BOARD_HAS_AXP2101
#include "axp_bsp.h"
#endif

#if BOARD_HAS_GPIO_POWER
#include "board_power_bsp.h"
#endif

#if BOARD_HAS_BAT_ADC
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

#define TAG "POWER"

#if BOARD_HAS_GPIO_POWER
/* 构造函数在静态初始化时就把 VBAT 自锁拉高（见 board_power_bsp.cpp）*/
static board_power_bsp_t s_board(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
#endif

#if BOARD_HAS_BAT_ADC

#define BAT_ADC_PIN  GPIO_NUM_4

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t         s_cali = NULL;
static adc_channel_t             s_chan;

static void adc_init(void)
{
    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(BAT_ADC_PIN, &unit, &s_chan) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = unit;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) { s_adc = NULL; return; }
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten    = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(s_adc, s_chan, &ccfg);

    adc_cali_curve_fitting_config_t cal = {};
    cal.unit_id  = unit;
    cal.chan     = s_chan;
    cal.atten    = ADC_ATTEN_DB_12;
    cal.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
}

int app_power_vbat_mv(void)
{
    if (!s_adc || !s_cali) return -1;
    int sum = 0, ok = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, s_chan, &raw) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) { sum += mv; ok++; }
    }
    return ok ? (sum / ok) * 2 : -1;
}

#elif BOARD_HAS_AXP2101   /* AMOLED 板：电池挂在 PMU 上，读寄存器就行，不走 ADC */

static void adc_init(void) {}
int app_power_vbat_mv(void) { return axp_bsp_vbat_mv(); }

#else

static void adc_init(void) {}
int app_power_vbat_mv(void) { return -1; }

#endif

void app_power_init(void)
{
#if BOARD_HAS_GPIO_POWER
    s_board.VBAT_POWER_ON();
    s_board.POWEER_EPD_ON();
    s_board.POWEER_Audio_ON();
    vTaskDelay(pdMS_TO_TICKS(10));
#endif

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_UNDEFINED)
        ESP_LOGI(TAG, "从深度睡眠唤醒（原因 %d）", (int)cause);

    adc_init();
#if BOARD_HAS_AXP2101
    ESP_LOGI(TAG, "电池 %d mV / %d%%  USB %s", app_power_vbat_mv(), axp_bsp_percent(),
             app_power_usb_connected() ? "已连接电脑" : "未连接电脑");
#else
    ESP_LOGI(TAG, "电池电压 %d mV  USB %s", app_power_vbat_mv(),
             app_power_usb_connected() ? "已连接电脑" : "未连接电脑");
#endif
}

bool app_power_usb_connected(void) { return usb_serial_jtag_is_connected(); }

#if BOARD_HAS_PWR_BUTTON
bool app_power_key_down(void) { return gpio_get_level(PWR_BUTTON_PIN) == 0; }
#else
bool app_power_key_down(void) { return false; }
#endif

void app_power_off(void)
{
#if BOARD_HAS_GPIO_POWER
    /* 等松开 PWR 键：按着的时候键本身就接通电源，断自锁也关不掉 */
    for (int i = 0; i < 250 && app_power_key_down(); i++) vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "关机：断开电池自锁");
    s_board.VBAT_POWER_OFF();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 还活着说明插着 USB 供电：关掉外设电源，深度睡眠，按 PWR 唤醒。
     * 不用 BOOT(GPIO0) 唤醒 —— 它是下载模式的 strapping 脚。*/
    ESP_LOGI(TAG, "USB 供电中，改为深度睡眠（按 PWR 唤醒）");
    s_board.POWEER_Audio_OFF();
    s_board.POWEER_EPD_OFF();
    rtc_gpio_pullup_en(PWR_BUTTON_PIN);
    rtc_gpio_pulldown_dis(PWR_BUTTON_PIN);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   /* 保持 RTC 上拉 */
    esp_sleep_enable_ext1_wakeup(1ULL << PWR_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
    vTaskDelay(pdMS_TO_TICKS(50));                                     /* 让日志发完 */
    esp_deep_sleep_start();
#elif BOARD_HAS_AXP2101
    /* AMOLED 板没有 GPIO 自锁，电源全在 AXP2101 手里：写一下软关机位，
     * 芯片自己切断所有输出。开机通路还没验通，见 axp_bsp.h 里那段。*/
    axp_bsp_shutdown();
#else
    ESP_LOGW(TAG, "这块板没有关机通路，本次忽略");
#endif
}
