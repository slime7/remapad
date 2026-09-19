#include "console_out.h"

#include <stdio.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cli.h"

static const char *TAG = "remapad_console";

/** UART0 的扩展焊盘引脚（hardware.md）：接 USB-UART 适配器看 host 模式日志。 */
#define CONSOLE_UART_NUM 0
#define CONSOLE_UART_TX_GPIO 43
#define CONSOLE_UART_RX_GPIO 44
#define CONSOLE_UART_BAUD 115200
#define CONSOLE_UART_BUF 1024
/* UART0 上的 CLI 与桥接 CLI 同一套命令表：amiibo 的 SPIFFS fopen 调用链
 * 栈深，3072 会溢出，与 remapad-input 对齐到 8192。 */
#define CONSOLE_RX_TASK_STACK 8192
#define CONSOLE_RX_TASK_PRIO 4

static bool s_uart_active;
static volatile bool s_rx_stop;
static TaskHandle_t s_rx_task;

/** 日志出口：格式化到栈上缓冲后写 UART0，绝不阻塞调用任务。 */
static int uart_vprintf(const char *fmt, va_list args)
{
    char line[192];
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n > 0) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        uart_write_bytes(CONSOLE_UART_NUM, line, len);
    }
    return n;
}

static void console_rx_task(void *param)
{
    (void)param;
    uint8_t buf[128];
    while (!s_rx_stop) {
        const int n = uart_read_bytes(CONSOLE_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            cli_feed_bytes(buf, (size_t)n);
        }
    }
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t console_out_use_uart0(void)
{
    if (s_uart_active) {
        return ESP_OK;
    }
    const uart_config_t config = {
        .baud_rate = CONSOLE_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(CONSOLE_UART_NUM, CONSOLE_UART_BUF, CONSOLE_UART_BUF, 0,
                                        NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(CONSOLE_UART_NUM, &config));
    ESP_ERROR_CHECK(uart_set_pin(CONSOLE_UART_NUM, CONSOLE_UART_TX_GPIO, CONSOLE_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* 日志改走这里；CLI 输出由 console_out_write 分流。 */
    esp_log_set_vprintf(uart_vprintf);
    s_rx_stop = false;
    if (xTaskCreate(console_rx_task, "remapad-uart", CONSOLE_RX_TASK_STACK, NULL,
                    CONSOLE_RX_TASK_PRIO, &s_rx_task) != pdPASS) {
        esp_log_set_vprintf(NULL);
        uart_driver_delete(CONSOLE_UART_NUM);
        return ESP_ERR_NO_MEM;
    }
    s_uart_active = true;
    ESP_LOGI(TAG, "console moved to UART0 (GPIO%d/%d, %d baud)", CONSOLE_UART_TX_GPIO,
             CONSOLE_UART_RX_GPIO, CONSOLE_UART_BAUD);
    return ESP_OK;
}

void console_out_use_usj(void)
{
    if (!s_uart_active) {
        return;
    }
    s_rx_stop = true;
    /* 等读任务自己退出（最多一个读超时）再卸驱动，避免它在读已释放的缓冲。 */
    for (int i = 0; i < 10 && s_rx_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rx_task != NULL) {
        vTaskDelete(s_rx_task);
        s_rx_task = NULL;
    }
    esp_log_set_vprintf(NULL);
    uart_driver_delete(CONSOLE_UART_NUM);
    s_uart_active = false;
}

bool console_out_uart_active(void)
{
    return s_uart_active;
}

void console_out_write(const char *text, size_t len)
{
    if (text == NULL || len == 0) {
        return;
    }
    if (s_uart_active) {
        uart_write_bytes(CONSOLE_UART_NUM, text, len);
        return;
    }
    /* 未切 UART0：stdout 就是 USJ 的非阻塞 vfs，未连接时丢弃。 */
    fwrite(text, 1, len, stdout);
    fflush(stdout);
}
