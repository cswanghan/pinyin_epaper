#include "touch_bsp.h"
#include "user_config.h"

#if !BOARD_HAS_TOUCH
/* 没有触摸的板子上整个退化成空壳 —— 组件在两块板上都会被编译 */
bool touch_bsp_init(touch_cb_t cb) { (void)cb; return false; }

#else

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "i2c_bsp.h"

#define TAG "TOUCH"

#define CST816_ADDR     0x15
#define REG_GESTURE     0x01      /* 0x01..0x06 连读：手势/手指数/X 高低/Y 高低 */
#define REG_CHIP_ID     0xA7
#define REG_IRQ_CTL     0xFA
#define REG_AUTOSLEEP   0xFE

/* CST816S=0xB4，CST816T=0xB5，CST816D=0xB6，CST820=0xB7。寄存器布局一样，都收 */
static bool chip_id_ok(uint8_t id) { return id >= 0xB4 && id <= 0xB7; }

/* 判手势的阈值。屏才 368 宽，滑动门槛定 60 px（约六分之一屏）——
 * 再小容易把「点歪了」当成滑动，再大孩子的小手划不够 */
#define SWIPE_MIN_PX     60
#define TAP_MAX_PX       25
#define TAP_MAX_MS      800
#define POLL_MS          25      /* 按住期间的采样间隔，只在手指在屏上时才跑 */

static touch_cb_t         s_cb   = NULL;
static SemaphoreHandle_t  s_sem  = NULL;

static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

/* 读一次触摸状态。返回是否有手指在屏上；有的话把坐标写进 x/y */
static bool read_point(int *x, int *y)
{
    uint8_t r[6] = {0};
    if (i2c_peek(CST816_ADDR, REG_GESTURE, r, sizeof(r)) != 0) return false;
    if (r[1] == 0) return false;                      /* FingerNum */
    *x = ((r[2] & 0x0F) << 8) | r[3];
    *y = ((r[4] & 0x0F) << 8) | r[5];
    return true;
}

static void touch_task(void *arg)
{
    for (;;) {
        /* 手指不在屏上时就停在这儿，一点 I2C 流量都不跑 */
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) continue;

        int x0 = 0, y0 = 0;
        if (!read_point(&x0, &y0)) continue;          /* 抖动，INT 来了但没按住 */

        int64_t t0 = esp_timer_get_time();
        int x = x0, y = y0, nx, ny;
        /* 跟到抬手为止。INT 只在按下时给一下，抬起得自己轮询出来 */
        while (read_point(&nx, &ny)) {
            x = nx; y = ny;
            vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        }
        int dx = x - x0, dy = y - y0;
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        int adx = abs(dx), ady = abs(dy);

        /* INT 在按住期间还会继续给信号，全堆在信号量里；抬手后清掉，
         * 不然一次触摸会连着触发好几轮 */
        xSemaphoreTake(s_sem, 0);

        touch_evt_t evt;
        if (adx >= SWIPE_MIN_PX && adx > ady) {
            evt = (dx < 0) ? TOUCH_SWIPE_LEFT : TOUCH_SWIPE_RIGHT;
        } else if (adx < TAP_MAX_PX && ady < TAP_MAX_PX && ms < TAP_MAX_MS) {
            evt = TOUCH_TAP;
        } else {
            ESP_LOGD(TAG, "忽略: dx=%d dy=%d %dms", dx, dy, ms);
            continue;                                  /* 不上不下的动作，不猜 */
        }
        ESP_LOGI(TAG, "%s (dx=%d dy=%d %dms)",
                 evt == TOUCH_TAP ? "轻点" : evt == TOUCH_SWIPE_LEFT ? "左滑" : "右滑",
                 dx, dy, ms);
        if (s_cb) s_cb(evt);
    }
}

bool touch_bsp_init(touch_cb_t cb)
{
    uint8_t id = 0;
    /* 刚放开复位，芯片要一会儿才应答；读不到就当没有触摸，不拖垮开机 */
    for (int i = 0; i < 5; i++) {
        if (i2c_peek(CST816_ADDR, REG_CHIP_ID, &id, 1) == 0 && chip_id_ok(id)) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!chip_id_ok(id)) {
        ESP_LOGW(TAG, "CST816 没应答（读到 ID=0x%02X），触摸不可用", id);
        return false;
    }

    i2c_poke(CST816_ADDR, REG_AUTOSLEEP, 0xFF);   /* 关自动休眠：睡着了第一下点不醒 */
    i2c_poke(CST816_ADDR, REG_IRQ_CTL, 0x40);     /* 只要「按下」这一种中断 */

    s_sem = xSemaphoreCreateBinary();
    if (!s_sem) return false;
    s_cb = cb;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << AMOLED_TOUCH_INT_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    /* 别的组件可能已经装过 ISR 服务了，重复装返回 INVALID_STATE，不是错误 */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(AMOLED_TOUCH_INT_PIN, int_isr, NULL);

    xTaskCreate(touch_task, "touch", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "CST816 就绪 ID=0x%02X：轻点=唤醒，左滑=下一个，右滑=上一个", id);
    return true;
}

#endif /* BOARD_HAS_TOUCH */
