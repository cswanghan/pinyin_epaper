/*****************************************************************************
 * 调试控制台实现
 *
 * sdkconfig 里主控制台是 UART0，USB-Serial-JTAG 只是"副控制台"（只输出），
 * stdin 读不到 USB 这边的输入。所以装 USB-Serial-JTAG 驱动直接读，
 * 同时让日志输出也改走驱动 —— 否则驱动和日志会抢同一个 FIFO。
 *****************************************************************************/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "app_console.h"
#include "app_sr.h"
#include "app_store.h"

#define TAG "CONSOLE"

static console_post_t s_post = NULL;

static const struct { const char *name; int cmd; } k_cmds[] = {
    {"n", SR_CMD_NEXT},       {"p", SR_CMD_PREV},
    {"r", SR_CMD_REPEAT},     {"w", SR_CMD_WORDS},
    {"m", SR_CMD_MASTERED},   {"f", SR_CMD_FORGOT},     {"v", SR_CMD_REVIEW},
    {"gn", APP_CMD_GROUP_NEXT}, {"gp", APP_CMD_GROUP_PREV},
    {"k", APP_EVT_WAKE_KEY},
    {"wake", SR_EVT_WAKE},    {"timeout", SR_EVT_TIMEOUT},
    {"s", APP_EVT_STATUS},    {"l", APP_EVT_LIST},
    {"export", APP_EVT_EXPORT}, {"off", APP_EVT_POWER_OFF},
};

static void run_line(char *s)
{
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len && s[len - 1] == ' ') s[--len] = 0;
    if (!len) return;
    ESP_LOGI(TAG, "> %s", s);

    for (const auto &c : k_cmds)
        if (strcmp(s, c.name) == 0) { s_post(c.cmd); return; }
    int n;
    if (sscanf(s, "g %d", &n) == 1 && n >= 1 && n <= GROUPS_MAX) { s_post(APP_CMD_GROUP_BASE + n); return; }
    if (strncmp(s, "c ", 2) == 0) {                /* 查字: c 如 / c 123 */
        const char *q = s + 2;
        while (*q == ' ') q++;
        char *end;
        long id = strtol(q, &end, 10);
        if (end == q || *end) id = app_groups_char_id(q);   /* 不是数字就当汉字查 */
        if (id >= 0 && id < STORE_MAX_IDS) { s_post(SR_CMD_CHAR_BASE + (int)id); return; }
        ESP_LOGI(TAG, "清单里没有「%s」", q);
        return;
    }
    if (strncmp(s, "py ", 3) == 0) {               /* 把拼音当成听到的原始文本查字: py ru guo de guo */
        int cmd = app_sr_lookup_text(s + 3);
        if (cmd >= SR_CMD_CHAR_BASE) s_post(cmd);
        return;
    }
    if (strncmp(s, "find ", 5) == 0) {
        /* 只读 SD 上的清单文件，不碰学习状态，直接在本任务里做 */
        const char *q = s + 5;
        while (*q == ' ') q++;
        if (*q) { app_groups_grep(q); return; }
    }
    ESP_LOGI(TAG, "命令: n p r w m f v | c 汉字或字id 查字 | py 拼音 按听到的拼音查字 | gn gp g N 切组 | l 本组字表 | find 汉字或拼音 | wake timeout s export off");
}

static void console_task(void *arg)
{
    char line[64];
    size_t len = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r' || ch == '\n') {            /* 兼容 CR / LF / CRLF */
            line[len] = 0;
            run_line(line);
            len = 0;
        } else if (ch == 8 || ch == 127) {
            if (len) len--;
        } else if (ch >= 0x20 && len < sizeof(line) - 1) {   /* 含 UTF-8，find 可以直接敲汉字 */
            line[len++] = (char)ch;
        }
    }
}

void app_console_start(console_post_t post)
{
    s_post = post;
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 1024;
    cfg.rx_buffer_size = 256;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB 串口驱动安装失败，控制台不可用");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    xTaskCreate(console_task, "console", 6 * 1024, NULL, 2, NULL);   /* find 要读 SD，py 要算对齐 */
    ESP_LOGI(TAG, "调试控制台就绪：在 idf.py monitor 里敲命令回车，敲 ? 看帮助");
}
