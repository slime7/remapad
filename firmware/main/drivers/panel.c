#include "panel.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 面板与引脚事实来自 docs/hardware.md；方向、GRAM 偏移与反转配置逐条对照微雪
 * 官方示例（02_ESP_IDF_ST7789_LVGL）：
 * https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/tree/main/examples/esp-idf/02_ESP_IDF_ST7789_LVGL
 *
 * 像素时钟取 SPI2 的上限 80 MHz：条带管线里传输本就基本被渲染盖住，取上限
 * 是为了压缩整帧重绘时的串行等待与 DMA 占线时间（整帧 240x280 约 13.5 ms，
 * 40 MHz 时约 27 ms）。取值依据见 docs/adr/0018。 */
#define REMAPAD_LCD_H_RES 240
#define REMAPAD_LCD_V_RES 280
#define REMAPAD_LCD_SPI_HOST SPI2_HOST
#define REMAPAD_LCD_PIXEL_CLK_HZ (80 * 1000 * 1000)
#define REMAPAD_LCD_CMD_BITS 8
#define REMAPAD_LCD_PARAM_BITS 8
/* 同步传输的等待上限：整帧 240x280 在 80 MHz 下约 13.5 ms，留足余量。 */
#define REMAPAD_PANEL_TIMEOUT_MS 200

#define REMAPAD_LCD_GPIO_SCLK 6
#define REMAPAD_LCD_GPIO_MOSI 7
#define REMAPAD_LCD_GPIO_RST 8
#define REMAPAD_LCD_GPIO_DC 4
#define REMAPAD_LCD_GPIO_CS 5

static const char *TAG = "driver_panel";
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
/* draw_bitmap 只把 DMA 事务排队就返回；每笔事务在最后一个分块完成时回调一次
 * on_color_trans_done，用它维护在飞笔数 s_color_pending，并唤醒等待者。 */
static SemaphoreHandle_t s_color_done = NULL;
static uint32_t s_color_pending = 0;
/* 提交与完成的单调序号：每笔提交领一个号，回调按提交顺序推进完成号。调用方
 * 用它判断「我上一笔用过的缓冲」是否已经空出来，从而把多笔传输排进队列，
 * 不必每笔都等到 DMA 结束。 */
static uint32_t s_color_submitted = 0;
static uint32_t s_color_completed = 0;

/* 面板字节序：RGB565 在内存里是小端，SPI 按字节流先发低地址字节、面板按大端
 * 解释像素，因此提交前要把每个像素换成大端序。按 32 位成对、八像素一批处理，
 * 整屏 6.7 万像素时这是除传输之外唯一必须逐像素走一遍的环节。 */
typedef uint32_t __attribute__((may_alias)) panel_pair_t;

/** 32 位整字里的两个 RGB565 像素各自换字节序，像素顺序保持不变。
 * 注意不能用 __builtin_bswap32：那会连同两个像素的前后顺序一起颠倒，实机表现
 * 为相邻像素成对交换（平坦色块看不出来，文字与圆弧会发糊、边缘上下波动）。 */
static inline uint32_t panel_byteswap_pair(uint32_t value)
{
    return ((value & 0x00FF00FFU) << 8) | ((value >> 8) & 0x00FF00FFU);
}

static void panel_byteswap_rgb565(uint16_t *pixels, size_t count)
{
    /* 缓冲按 64 字节对齐申请，奇像素前缀只可能来自非对齐调用方。 */
    if (((uintptr_t)pixels & (uintptr_t)2U) != 0U && count != 0U) {
        pixels[0] = (uint16_t)__builtin_bswap16(pixels[0]);
        pixels++;
        count--;
    }
    size_t index = 0;
    for (; index + 8U <= count; index += 8U) {
        panel_pair_t *pairs = (panel_pair_t *)(void *)(pixels + index);
        pairs[0] = panel_byteswap_pair(pairs[0]);
        pairs[1] = panel_byteswap_pair(pairs[1]);
        pairs[2] = panel_byteswap_pair(pairs[2]);
        pairs[3] = panel_byteswap_pair(pairs[3]);
    }
    for (; index + 2U <= count; index += 2U) {
        panel_pair_t *pair = (panel_pair_t *)(void *)(pixels + index);
        *pair = panel_byteswap_pair(*pair);
    }
    if (index < count) {
        pixels[index] = (uint16_t)__builtin_bswap16(pixels[index]);
    }
}

/* 启动时自检批量换序与逐像素参考实现是否一致；不一致只记日志，不阻断启动
 * （换序错了整幅图会成对错位，必须能立刻看见）。 */
static void panel_byteswap_selftest(void)
{
    uint16_t sample[8] = { 0x1234U, 0xABCDU, 0x0000U, 0xFFFFU,
                           0x07E0U, 0x001FU, 0x8410U, 0x5A5AU };
    uint16_t expected[8];
    for (size_t index = 0; index < 8U; ++index) {
        expected[index] = (uint16_t)__builtin_bswap16(sample[index]);
    }
    panel_byteswap_rgb565(sample, 8U);
    for (size_t index = 0; index < 8U; ++index) {
        if (sample[index] != expected[index]) {
            ESP_LOGE(TAG, "byte swap self-check failed at %u: %04x != %04x",
                     (unsigned)index, sample[index], expected[index]);
            return;
        }
    }
    ESP_LOGI(TAG, "byte swap self-check ok");
}

static bool IRAM_ATTR panel_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                             esp_lcd_panel_io_event_data_t *edata,
                                             void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    (void)__atomic_fetch_sub(&s_color_pending, 1U, __ATOMIC_RELAXED);
    (void)__atomic_fetch_add(&s_color_completed, 1U, __ATOMIC_RELEASE);
    BaseType_t higher_priority_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_color_done, &higher_priority_woken);
    return higher_priority_woken == pdTRUE;
}

esp_err_t panel_init(void)
{
    if (s_panel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    panel_byteswap_selftest();
    if (s_color_done == NULL) {
        s_color_done = xSemaphoreCreateBinary();
        if (s_color_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
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
        .on_color_trans_done = panel_color_trans_done,
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

static esp_err_t panel_submit(uint16_t *pixels, int x, int y, int width, int height,
                              uint32_t *out_seq)
{
    if (s_panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pixels == NULL || width <= 0 || height <= 0 || x < 0 || y < 0 ||
        x + width > REMAPAD_LCD_H_RES || y + height > REMAPAD_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t count = (size_t)width * (size_t)height;
    panel_byteswap_rgb565(pixels, count);

    /* esp_lcd 的 draw_bitmap 使用开区间终点。它把 CASET/RASET 命令与颜色数据
     * 排进 SPI 队列后即返回，颜色分块的 DMA 可能仍在飞行；官方约定颜色缓冲
     * 必须在 on_color_trans_done 之后才能复用，因此先记在飞笔数再提交，保证
     * 回调的减计数不会跑到计数的前面。 */
    (void)__atomic_fetch_add(&s_color_pending, 1U, __ATOMIC_RELEASE);
    const uint32_t seq = __atomic_add_fetch(&s_color_submitted, 1U, __ATOMIC_RELAXED);
    esp_err_t result =
        esp_lcd_panel_draw_bitmap(s_panel, x, y, x + width, y + height, pixels);
    if (result != ESP_OK) {
        (void)__atomic_fetch_sub(&s_color_pending, 1U, __ATOMIC_RELAXED);
        return result;
    }
    if (out_seq != NULL) {
        *out_seq = seq;
    }
    return ESP_OK;
}

esp_err_t panel_transfer_wait(uint32_t timeout_ms)
{
    return panel_wait_seq(__atomic_load_n(&s_color_submitted, __ATOMIC_ACQUIRE),
                          timeout_ms);
}

esp_err_t panel_wait_seq(uint32_t seq, uint32_t timeout_ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (__atomic_load_n(&s_color_completed, __ATOMIC_ACQUIRE) < seq) {
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            ESP_LOGE(TAG, "panel transfer did not finish in time");
            return ESP_ERR_TIMEOUT;
        }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remaining / 1000));
        if (ticks == 0) {
            ticks = 1;
        }
        (void)xSemaphoreTake(s_color_done, ticks);
    }
    return ESP_OK;
}

esp_err_t panel_transfer(uint16_t *pixels, int x, int y, int width, int height)
{
    ESP_RETURN_ON_ERROR(panel_submit(pixels, x, y, width, height, NULL),
                        TAG, "panel submit failed");
    return panel_transfer_wait(REMAPAD_PANEL_TIMEOUT_MS);
}

esp_err_t panel_transfer_async(uint16_t *pixels, int x, int y, int width, int height,
                               uint32_t *out_seq)
{
    return panel_submit(pixels, x, y, width, height, out_seq);
}
