#include "panel.h"

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "driver/spi_master.h"

/* 面板与引脚事实来自 docs/hardware.md；方向、GRAM 偏移与反转配置逐条对照微雪
 * 官方示例（02_ESP_IDF_ST7789_LVGL）：
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/tree/main/examples/esp-idf/02_ESP_IDF_ST7789_LVGL */
#define REMAPAD_LCD_H_RES 240
#define REMAPAD_LCD_V_RES 280
#define REMAPAD_LCD_SPI_HOST SPI2_HOST
#define REMAPAD_LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define REMAPAD_LCD_CMD_BITS 8
#define REMAPAD_LCD_PARAM_BITS 8

#define REMAPAD_LCD_GPIO_SCLK 6
#define REMAPAD_LCD_GPIO_MOSI 7
#define REMAPAD_LCD_GPIO_RST 8
#define REMAPAD_LCD_GPIO_DC 4
#define REMAPAD_LCD_GPIO_CS 5

static const char *TAG = "driver_panel";
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

esp_err_t panel_init(void)
{
    if (s_panel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 面板只有 DIN 写入线，MISO 悬空；最大传输按整帧预留。 */
    const spi_bus_config_t bus_config = {
        .sclk_io_num = REMAPAD_LCD_GPIO_SCLK,
        .mosi_io_num = REMAPAD_LCD_GPIO_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = REMAPAD_LCD_H_RES * REMAPAD_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(REMAPAD_LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "init SPI bus failed");

    /* PSRAM 颜色缓冲经 EDMA 直读，省去驱动侧内部搬运缓冲。 */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = REMAPAD_LCD_GPIO_DC,
        .cs_gpio_num = REMAPAD_LCD_GPIO_CS,
        .pclk_hz = REMAPAD_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = REMAPAD_LCD_CMD_BITS,
        .lcd_param_bits = REMAPAD_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .flags =
            {
                .psram_dma_direct = 1,
            },
    };
    esp_err_t result = esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)REMAPAD_LCD_SPI_HOST, &io_config, &s_io);
    if (result != ESP_OK) {
        goto fail;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = REMAPAD_LCD_GPIO_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    result = esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel);
    if (result != ESP_OK) {
        goto fail;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, false, false), TAG, "panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "panel invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 20), TAG, "panel set gap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "panel display on failed");
    return ESP_OK;

fail:
    if (s_panel != NULL) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_io != NULL) {
        esp_lcd_panel_io_del(s_io);
        s_io = NULL;
    }
    spi_bus_free(REMAPAD_LCD_SPI_HOST);
    return result;
}

esp_err_t panel_transfer(uint16_t *pixels, int x, int y, int width, int height)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > REMAPAD_LCD_H_RES || y + height > REMAPAD_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    /* RGB565 寄存器在内存里是小端，SPI 按字节流先发低地址字节，面板按大端
     * 解释像素，因此传输前把每个像素换成大端序。 */
    const size_t count = (size_t)width * (size_t)height;
    for (size_t index = 0; index < count; ++index) {
        pixels[index] = (uint16_t)__builtin_bswap16(pixels[index]);
    }

    /* esp_lcd 的 draw_bitmap 使用开区间终点。每次 draw 都会重新
     * spi_device_acquire_bus，而 acquire 按 spi_master.h 的约定会先排空该设备
     * 上一轮排队的事务，因此调用方复用同一缓冲在下一帧重写是安全的，
     * 无需额外的传输完成门控。 */
    return esp_lcd_panel_draw_bitmap(s_panel, x, y, x + width, y + height, pixels);
}
