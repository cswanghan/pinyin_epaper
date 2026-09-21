#include "axp_bsp.h"
#include "user_config.h"

#if !BOARD_HAS_AXP2101
/* 墨水屏板没有 PMU，整个退化成空壳 —— 组件在两块板上都会被编译 */
bool axp_bsp_init(void)            { return false; }
void axp_bsp_dump(const char *why) { (void)why; }
void axp_bsp_key_start(axp_key_cb_t cb) { (void)cb; }
void axp_bsp_shutdown(void)        {}
int  axp_bsp_vbat_mv(void)         { return -1; }
int  axp_bsp_percent(void)         { return -1; }
#if AXP_PROBE
void axp_bsp_probe_start(void)     {}
#endif

#else

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2c_bsp.h"

#define TAG "AXP"

#define AXP_ADDR      0x34
#define REG_IC_TYPE   0x03      /* 芯片型号，实测读出 0x4A */
#define REG_COMMON    0x10      /* 通用配置，底档 0x34；bit0 = 软关机 */
#define REG_VBAT_H    0x34      /* 电池电压，高字节在前，只有低 6 位是数据 */
#define REG_IRQ_ST0   0x48      /* 中断状态 0..2，写 1 清零 */
#define REG_SOC       0xA4      /* 电量百分比 */

/* PKEY 的位都在 0x49 上（怎么探出来的见 axp_bsp.h）*/
#define REG_PKEY      0x49
#define PKEY_UP       0x01      /* bit0 松手 */
#define PKEY_DOWN     0x02      /* bit1 按下 */

/* 30 ms 一次。实测按一下最短的按下→松手间隔是 190 ms，轮这么快一下都漏不掉，
 * 而每次只是读一个字节，100 kHz 上大概 0.3 ms，忽略不计。*/
#define KEY_POLL_MS   30
#define KEY_LONG_MS 1200        /* 按住多久算长按。芯片自己那个 1.5 秒阈值不用，固件说了算 */

/* 扫描范围。0x80 往上是 DCDC/LDO 的使能和电压，探针连读都不去碰 ——
 * 扫描本身是安全的，但把那片值打在日志里容易诱着人去「照着写回去」。*/
#define SCAN_LO   0x00
#define SCAN_HI   0x7F

static bool rd(uint8_t reg, uint8_t *v)
{
    return i2c_peek(AXP_ADDR, reg, v, 1) == 0;
}

void axp_bsp_dump(const char *why)
{
    char line[80];
    ESP_LOGI(TAG, "寄存器快照（%s）", why ? why : "");
    for (int base = SCAN_LO; base <= SCAN_HI; base += 16) {
        int n = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t v = 0;
            if (rd(base + i, &v)) n += snprintf(line + n, sizeof(line) - n, " %02X", v);
            else                  n += snprintf(line + n, sizeof(line) - n, " --");
        }
        ESP_LOGI(TAG, "  %02X:%s", base, line);
    }
    /* 0x80..0x9F 是各路 DCDC/LDO 的使能和电压，故意不打 —— 那几个值写错能把主控喂到
     * 3.6V，不该出现在随手可抄的日志里。但 0xA0 往上是电量计，读一眼没有代价。*/
    for (int base = 0xA0; base <= 0xAF; base += 16) {
        int n = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t v = 0;
            if (rd(base + i, &v)) n += snprintf(line + n, sizeof(line) - n, " %02X", v);
            else                  n += snprintf(line + n, sizeof(line) - n, " --");
        }
        ESP_LOGI(TAG, "  %02X:%s", base, line);
    }
}

bool axp_bsp_init(void)
{
    uint8_t id = 0;
    if (!rd(REG_IC_TYPE, &id)) {
        ESP_LOGW(TAG, "AXP2101 没应答（I2C 0x%02X）", AXP_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "AXP2101 在线，芯片型号寄存器 0x%02X = 0x%02X", REG_IC_TYPE, id);
    return true;
}

int axp_bsp_vbat_mv(void)
{
    uint8_t hi = 0, lo = 0;
    if (!rd(REG_VBAT_H, &hi) || !rd(REG_VBAT_H + 1, &lo)) return -1;
    int mv = ((hi & 0x3F) << 8) | lo;          /* 高字节上面两位不是数据 */
    return (mv > 2000 && mv < 5000) ? mv : -1; /* 锂电池不可能在这个区间外，越界就当没读到 */
}

int axp_bsp_percent(void)
{
    uint8_t v = 0;
    if (!rd(REG_SOC, &v)) return -1;
    return v <= 100 ? v : -1;
}

void axp_bsp_shutdown(void)
{
    uint8_t v = 0;
    if (!rd(REG_COMMON, &v)) { ESP_LOGE(TAG, "读不到 0x%02X，关不了机", REG_COMMON); return; }
    ESP_LOGW(TAG, "软关机：0x%02X %02X -> %02X", REG_COMMON, v, (uint8_t)(v | 0x01));
    vTaskDelay(pdMS_TO_TICKS(50));             /* 让这行日志发完 —— 下一句之后就没电了 */
    i2c_poke(AXP_ADDR, REG_COMMON, v | 0x01);
    vTaskDelay(pdMS_TO_TICKS(3000));
    /* 正常情况下上面那句之后主控就没电了，跑不到这儿。跑到了说明这一位不管用 ——
     * 换板子或者 AXP 型号不同才会这样，至少得让人知道机器没关掉。*/
    ESP_LOGE(TAG, "写了 0x%02X bit0 却没断电，这块板的关机位不在这儿", REG_COMMON);
}

/* ---------------- PWR 键 ---------------- */

static axp_key_cb_t s_key_cb = NULL;

static void key_task(void *arg)
{
    int64_t down_us = 0;
    bool    down = false, fired = false;

    /* 开机那一下按键早把中断位锁上了，不清掉的话第一次真按键看不到 0→1 跳变 */
    for (int r = REG_IRQ_ST0; r <= REG_IRQ_ST0 + 2; r++) i2c_poke(AXP_ADDR, r, 0xFF);

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));

        uint8_t v = 0;
        if (rd(REG_PKEY, &v) && v) {
            i2c_poke(AXP_ADDR, REG_PKEY, v);   /* 写 1 清零，不清就只看得见第一次 */
            if (v & PKEY_DOWN) { down = true; fired = false; down_us = esp_timer_get_time(); }
            if ((v & PKEY_UP) && down) {
                int ms = (int)((esp_timer_get_time() - down_us) / 1000);
                down = false;
                if (!fired) {                  /* 长按在还按着的时候就报过了，别报第二遍 */
                    ESP_LOGI(TAG, "PWR 键短按（%d ms）", ms);
                    if (s_key_cb) s_key_cb(AXP_KEY_SHORT);
                }
            }
        }

        /* 还按着就已经够长按时间的，当场就报。按电源键的人是盯着屏等反应的，
         * 非要等松手才动手，手感上像是没按到，会一直按下去。*/
        if (down && !fired && esp_timer_get_time() - down_us >= (int64_t)KEY_LONG_MS * 1000) {
            fired = true;
            ESP_LOGI(TAG, "PWR 键长按（%d ms）", KEY_LONG_MS);
            if (s_key_cb) s_key_cb(AXP_KEY_LONG);
        }
    }
}

void axp_bsp_key_start(axp_key_cb_t cb)
{
    s_key_cb = cb;
    xTaskCreate(key_task, "axp_key", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "PWR 键就绪：短按=熄屏/亮屏，按住 %d ms=关机", KEY_LONG_MS);
}

#if AXP_PROBE

/* 只读探针。
 *
 * 扫 0x00..0x4F：前面是各种状态位（电池在不在、USB 插没插、在不在充电），
 * 0x40 往后按 X-Powers 一贯的排法是中断使能和中断状态。中断状态位是「锁存」的，
 * 按一下 PWR 键置上就不会自己掉，所以扫得慢一点也漏不掉。
 *
 * 一个字节都不写 —— 包括不清中断。代价是同一位只会报一次 0→1，
 * 所以试的时候要「短按、等几秒、长按」，两次变化才分得开是哪一位。
 */
#define PROBE_LO   0x00
#define PROBE_HI   0x4F

/* 中断状态那三个寄存器单独快轮，20 ms 一遍，短按也漏不掉；
 * 其余寄存器一秒扫一遍就够（电池、充电状态都是慢变量）。*/
#define IRQ_LO     0x48
#define IRQ_HI     0x4A
#define FAST_MS    20
#define SLOW_EVERY 50      /* 每 50 次快轮 = 1 秒，扫一遍其余寄存器 */

/* 电池电压、充电电流这些 ADC 结果每次采样都在动，不压住会把按键那一位淹在刷屏里。
 * 同一个寄存器变够 NOISY_AFTER 次就判成「一直在变」，此后只统计不再打印。*/
#define NOISY_AFTER  6

static void probe_task(void *arg)
{
    static uint8_t prev[PROBE_HI + 1];
    static bool    have[PROBE_HI + 1];
    static uint8_t hits[PROBE_HI + 1];

    for (int r = PROBE_LO; r <= PROBE_HI; r++) have[r] = rd(r, &prev[r]);

    ESP_LOGW(TAG, "探针已启动");

    /* 开机那一下按键早把中断位锁上了（0x48..0x4A 一上来就非零），不清掉的话
     * 再按 PWR 也看不到 0→1 跳变。X-Powers 这一族的中断状态都是「写 1 清零」。
     *
     * 这是整个探针里唯一的写操作。赌的是 0x48..0x4A 确实是中断状态寄存器 ——
     * 旁证有三条: 芯片 ID 0x03 读出 0x4A 对上 AXP2101；0x34/0x35 读出来正好是
     * 4102 mV 这个合理的电池电压；0x40..0x42 读出 FF FC 5F 一看就是「中断使能、
     * 默认全开」。而且就算猜错，这一片也只是充电参数，不是喂主控的电压寄存器，
     * 最坏的结果是关机，拔一下电池就回来。*/
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (int r = 0x48; r <= 0x4A; r++) i2c_poke(AXP_ADDR, r, 0xFF);
    for (int r = 0x48; r <= 0x4A; r++) have[r] = rd(r, &prev[r]);
    ESP_LOGW(TAG, "中断状态已清: 0x48=%02X 0x49=%02X 0x4A=%02X", prev[0x48], prev[0x49], prev[0x4A]);
    ESP_LOGW(TAG, "===> 现在开始: 短按 PWR 键一下，等 3 秒，再按住 PWR 键约 2 秒松开");

    int tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(FAST_MS));

        /* 中断状态：变了就打印，然后把刚看见的那几位写回去清掉。
         * 上一轮栽的就是这儿 —— 不清的话同一位只报一次 0→1，
         * 按第二下、第三下全都看不见，日志里就只剩孤零零一条。*/
        for (int r = IRQ_LO; r <= IRQ_HI; r++) {
            uint8_t v;
            if (!rd(r, &v)) continue;
            if (have[r] && v == prev[r]) continue;
            uint8_t was = have[r] ? prev[r] : 0;
            ESP_LOGW(TAG, "[%d ms] 0x%02X: %02X -> %02X   置位=%02X",
                     (int)(esp_timer_get_time() / 1000), r, was, v, (uint8_t)(v & ~was));
            if (v) i2c_poke(AXP_ADDR, r, v);          /* 写 1 清零 */
            have[r] = rd(r, &prev[r]);
        }

        if (++tick < SLOW_EVERY) continue;
        tick = 0;

        for (int r = PROBE_LO; r <= PROBE_HI; r++) {
            uint8_t v;
            if (r >= IRQ_LO && r <= IRQ_HI) continue;  /* 上面已经快轮过了 */
            if (!rd(r, &v)) continue;
            if (have[r] && v == prev[r]) continue;

            uint8_t was = have[r] ? prev[r] : 0;
            prev[r] = v; have[r] = true;

            if (hits[r] > NOISY_AFTER) continue;              /* 早就判成噪声了 */
            if (++hits[r] > NOISY_AFTER) {
                ESP_LOGW(TAG, "0x%02X 一直在变（八成是 ADC 采样值），以后不再报", r);
                continue;
            }
            /* 把变化拆到位上打出来，省得自己数二进制 */
            ESP_LOGW(TAG, "0x%02X: %02X -> %02X   置位=%02X 清位=%02X",
                     r, was, v, (uint8_t)(v & ~was), (uint8_t)(was & ~v));
        }
    }
}

void axp_bsp_probe_start(void)
{
    axp_bsp_dump("开机底档");
    xTaskCreate(probe_task, "axp_probe", 3072, NULL, 2, NULL);
}

#endif /* AXP_PROBE */
#endif /* BOARD_HAS_AXP2101 */
