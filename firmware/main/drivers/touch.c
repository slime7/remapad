#include "touch.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"

/* 引脚与地址来自 docs/hardware.md；触摸与 IMU、RTC 共享 GPIO10/11 上的
 * I2C 总线，靠 0x15 地址区分。I2C 配置对照微雪官方示例
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/tree/main/examples/esp-idf/02_ESP_IDF_ST7789_LVGL */
#define REMAPAD_TOUCH_I2C_PORT 0
#define REMAPAD_TOUCH_GPIO_SCL 10
#define REMAPAD_TOUCH_GPIO_SDA 11
#define REMAPAD_TOUCH_GPIO_RST 13
#define REMAPAD_TOUCH_GPIO_INT 14
#define REMAPAD_TOUCH_X_MAX 240
#define REMAPAD_TOUCH_Y_MAX 280

static const char *TAG = "driver_touch";
static esp_lcd_touch_handle_t s_touch = NULL;

esp_err_t touch_init(void)
{
    if (s_touch != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_master_bus_config_t bus_config = {
        .i2c_port = REMAPAD_TOUCH_I2C_PORT,
        .sda_io_num = REMAPAD_TOUCH_GPIO_SDA,
        .scl_io_num = REMAPAD_TOUCH_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags =
            {
                .enable_internal_pullup = true,
            },
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &bus), TAG, "init I2C bus failed");

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_config =
        ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_err_t result = esp_lcd_new_panel_io_i2c(bus, &io_config, &io);
    if (result != ESP_OK) {
        goto fail;
    }

    /* 面板经 mirror(true,true) 摆正后，触摸原始坐标系与显示一致，无需交换
     * 或镜像。 */
    const esp_lcd_touch_config_t touch_config = {
        .x_max = REMAPAD_TOUCH_X_MAX,
        .y_max = REMAPAD_TOUCH_Y_MAX,
        .rst_gpio_num = REMAPAD_TOUCH_GPIO_RST,
        .int_gpio_num = REMAPAD_TOUCH_GPIO_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };
    result = esp_lcd_touch_new_i2c_cst816s(io, &touch_config, &s_touch);
    if (result != ESP_OK) {
        goto fail;
    }
    return ESP_OK;

fail:
    if (io != NULL) {
        esp_lcd_panel_io_del(io);
    }
    i2c_del_master_bus(bus);
    return result;
}

size_t touch_sample(touch_contact_t *out, size_t capacity)
{
    if (s_touch == NULL || out == NULL || capacity == 0) {
        return 0;
    }

    /* CST816T 是单点触摸，读到按下即返回一个触点；无按下时触点数组清空。 */
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t count = 0;
    esp_lcd_touch_read_data(s_touch);
    const bool pressed =
        esp_lcd_touch_get_coordinates(s_touch, &x, &y, NULL, &count, 1);
    if (!pressed || count == 0) {
        return 0;
    }
    out[0].x = x;
    out[0].y = y;
    return 1;
}
