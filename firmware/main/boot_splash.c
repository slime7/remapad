#include "boot_splash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "backlight.h"
#include "panel.h"

static const char *TAG = "boot_splash";

/* 配色与 ui/src/theme.ts 的 MD3 token 同值：固件不复用前端常量，改动时两边
 * 一起看（这里只用到背景、机身、主色、次色、描边与错误色）。 */
#define SPLASH_RGB565(r, g, b) \
    ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

#define SPLASH_COLOR_BACKGROUND SPLASH_RGB565(0x06, 0x0f, 0x1b)
#define SPLASH_COLOR_SURFACE SPLASH_RGB565(0x10, 0x20, 0x35)
#define SPLASH_COLOR_PRIMARY SPLASH_RGB565(0xb8, 0xdb, 0xff)
#define SPLASH_COLOR_TERTIARY SPLASH_RGB565(0xfd, 0x9a, 0xce)
#define SPLASH_COLOR_OUTLINE SPLASH_RGB565(0x37, 0x49, 0x62)
#define SPLASH_COLOR_ERROR SPLASH_RGB565(0xff, 0x71, 0x6c)

/* 画面几何（逻辑像素，与 host profile 的 240x280 视口一致）：机身圆角矩形
 * 里左侧是十字键、右侧是四个按键点，下方是阶段进度条。 */
#define SPLASH_WIDTH 240
#define SPLASH_HEIGHT 280
#define SPLASH_MARK_X 68
#define SPLASH_MARK_Y 96
#define SPLASH_MARK_W 104
#define SPLASH_MARK_H 60
#define SPLASH_MARK_RADIUS 20
#define SPLASH_CONTROLS_CY (SPLASH_MARK_Y + 30)
#define SPLASH_DPAD_CX (SPLASH_MARK_X + 24)
#define SPLASH_DPAD_HALF 12
#define SPLASH_DPAD_THICK 6
/* 四个按键点贴机身右侧：中心留出点半径 + 14 px，与左侧十字键的 12 px 边距接近。 */
#define SPLASH_DOTS_CX (SPLASH_MARK_X + SPLASH_MARK_W - 32)
#define SPLASH_DOT_COUNT 4
#define SPLASH_DOT_OFFSET_X 14
#define SPLASH_DOT_OFFSET_Y 16
#define SPLASH_DOT_RADIUS 4
#define SPLASH_TRACK_X 40
#define SPLASH_TRACK_Y 196
#define SPLASH_TRACK_W 160
#define SPLASH_TRACK_H 8
#define SPLASH_TRACK_RADIUS 4

/* 动态区域：阶段推进只重画这四个按键点与进度条，区域自身的外接矩形就是传输
 * 窗口，底色按各自所在位置取机身色或页面背景色。 */
#define SPLASH_DOTS_X (SPLASH_DOTS_CX - SPLASH_DOT_OFFSET_X - SPLASH_DOT_RADIUS)
#define SPLASH_DOTS_Y (SPLASH_CONTROLS_CY - SPLASH_DOT_OFFSET_Y - SPLASH_DOT_RADIUS)
#define SPLASH_DOTS_W (2 * (SPLASH_DOT_OFFSET_X + SPLASH_DOT_RADIUS))
#define SPLASH_DOTS_H (2 * (SPLASH_DOT_OFFSET_Y + SPLASH_DOT_RADIUS))

/* 边缘覆盖率：每像素 4x4 子采样，采样点位置用 1/8 像素的整数单位表示，
 * 这样圆角与圆点边缘不需要浮点就有抗锯齿。 */
#define SPLASH_SUBSAMPLES 4
#define SPLASH_AA_UNITS 8
#define SPLASH_AA_SAMPLE(sample) (2 * (sample) + 1)
#define SPLASH_AA_SAMPLES (SPLASH_SUBSAMPLES * SPLASH_SUBSAMPLES)

/* 阶段权重表容量与动画刷新周期：进度条按「阶段预计耗时 + 阶段内经过时间」
 * 推进，长阶段（guest 创建、mount、eval）期间条子持续前进而不是停在格上。 */
#define SPLASH_STAGE_MAX 24
#define SPLASH_ANIM_MS 100

/** 像素缓冲 + 它在整屏逻辑坐标里的位置；所有绘制都按逻辑坐标并裁到这个窗口。 */
typedef struct {
    uint16_t *pixels;
    int width;
    int height;
    int origin_x;
    int origin_y;
    uint16_t base_color;
} splash_canvas_t;

/** 圆角矩形；四个半径相等时即圆，圆点用 x = cx - r 的方形外接框表达。 */
typedef struct {
    int x;
    int y;
    int w;
    int h;
    int radius;
} splash_shape_t;

typedef struct {
    uint16_t *dots;
    uint16_t *bar;
    int filled;
    int highlight;
    uint16_t fill_color;
    int stage;
    size_t stage_count;
    uint32_t stage_base_ms;
    uint32_t total_ms;
    uint32_t stage_ms[SPLASH_STAGE_MAX];
    int64_t stage_started_us;
    bool active;
    bool failed;
    bool anim_running;
} splash_state_t;

static splash_state_t s_splash;

static int splash_clamp(int value, int low, int high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

/** 按覆盖率在 5/6/5 三通道上混合，不需要浮点。 */
static uint16_t splash_blend(uint16_t dst, uint16_t src, unsigned covered, unsigned samples)
{
    const unsigned inverse = samples - covered;
    const unsigned half = samples / 2;
    const unsigned red =
        ((((src >> 11) & 0x1fu) * covered) + (((dst >> 11) & 0x1fu) * inverse) + half) / samples;
    const unsigned green =
        ((((src >> 5) & 0x3fu) * covered) + (((dst >> 5) & 0x3fu) * inverse) + half) / samples;
    const unsigned blue =
        (((src & 0x1fu) * covered) + ((dst & 0x1fu) * inverse) + half) / samples;
    return (uint16_t)((red << 11) | (green << 5) | blue);
}

static unsigned splash_shape_coverage(int px, int py, const splash_shape_t *shape)
{
    const int left = shape->x * SPLASH_AA_UNITS;
    const int top = shape->y * SPLASH_AA_UNITS;
    const int right = (shape->x + shape->w) * SPLASH_AA_UNITS;
    const int bottom = (shape->y + shape->h) * SPLASH_AA_UNITS;
    const int radius = shape->radius * SPLASH_AA_UNITS;
    unsigned covered = 0;
    for (int sy = 0; sy < SPLASH_SUBSAMPLES; ++sy) {
        const int sample_y = py * SPLASH_AA_UNITS + SPLASH_AA_SAMPLE(sy);
        int dy = 0;
        if (sample_y < top + radius) {
            dy = top + radius - sample_y;
        } else if (sample_y > bottom - radius) {
            dy = sample_y - (bottom - radius);
        }
        for (int sx = 0; sx < SPLASH_SUBSAMPLES; ++sx) {
            const int sample_x = px * SPLASH_AA_UNITS + SPLASH_AA_SAMPLE(sx);
            int dx = 0;
            if (sample_x < left + radius) {
                dx = left + radius - sample_x;
            } else if (sample_x > right - radius) {
                dx = sample_x - (right - radius);
            }
            if (dx * dx + dy * dy <= radius * radius) {
                ++covered;
            }
        }
    }
    return covered;
}

static splash_shape_t splash_circle_shape(int cx, int cy, int radius)
{
    const splash_shape_t shape = {
        .x = cx - radius,
        .y = cy - radius,
        .w = radius * 2,
        .h = radius * 2,
        .radius = radius,
    };
    return shape;
}

static void splash_fill_rect(const splash_canvas_t *canvas, int x, int y, int w, int h,
                             uint16_t color)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    const int left = splash_clamp(x, canvas->origin_x, canvas->origin_x + canvas->width);
    const int right = splash_clamp(x + w, canvas->origin_x, canvas->origin_x + canvas->width);
    const int top = splash_clamp(y, canvas->origin_y, canvas->origin_y + canvas->height);
    const int bottom = splash_clamp(y + h, canvas->origin_y, canvas->origin_y + canvas->height);
    for (int py = top; py < bottom; ++py) {
        uint16_t *row = canvas->pixels + (size_t)(py - canvas->origin_y) * (size_t)canvas->width;
        for (int px = left; px < right; ++px) {
            row[px - canvas->origin_x] = color;
        }
    }
}

static void splash_fill_shape(const splash_canvas_t *canvas, splash_shape_t shape, uint16_t color)
{
    if (shape.w <= 0 || shape.h <= 0) {
        return;
    }
    const int limit = (shape.w < shape.h ? shape.w : shape.h) / 2;
    if (shape.radius > limit) {
        shape.radius = limit;
    }
    if (shape.radius < 0) {
        shape.radius = 0;
    }
    const int left = splash_clamp(shape.x, canvas->origin_x, canvas->origin_x + canvas->width);
    const int right = splash_clamp(shape.x + shape.w, canvas->origin_x, canvas->origin_x + canvas->width);
    const int top = splash_clamp(shape.y, canvas->origin_y, canvas->origin_y + canvas->height);
    const int bottom = splash_clamp(shape.y + shape.h, canvas->origin_y, canvas->origin_y + canvas->height);
    for (int py = top; py < bottom; ++py) {
        uint16_t *row = canvas->pixels + (size_t)(py - canvas->origin_y) * (size_t)canvas->width;
        for (int px = left; px < right; ++px) {
            const unsigned covered = splash_shape_coverage(px, py, &shape);
            if (covered == 0U) {
                continue;
            }
            uint16_t *pixel = row + (px - canvas->origin_x);
            *pixel = covered == SPLASH_AA_SAMPLES
                         ? color
                         : splash_blend(*pixel, color, covered, SPLASH_AA_SAMPLES);
        }
    }
}

static void splash_clear(const splash_canvas_t *canvas)
{
    splash_fill_rect(canvas, canvas->origin_x, canvas->origin_y, canvas->width, canvas->height,
                     canvas->base_color);
}

/** 四个按键点：highlight 指定的点用 tertiaryContainer，其余用 outlineVariant。 */
static void splash_paint_dots(const splash_canvas_t *canvas, int highlight)
{
    static const int offsets[SPLASH_DOT_COUNT][2] = {
        {0, -SPLASH_DOT_OFFSET_Y},
        {SPLASH_DOT_OFFSET_X, 0},
        {0, SPLASH_DOT_OFFSET_Y},
        {-SPLASH_DOT_OFFSET_X, 0},
    };
    for (int index = 0; index < SPLASH_DOT_COUNT; ++index) {
        const splash_shape_t dot = splash_circle_shape(
            SPLASH_DOTS_CX + offsets[index][0],
            SPLASH_CONTROLS_CY + offsets[index][1],
            SPLASH_DOT_RADIUS);
        splash_fill_shape(canvas, dot,
                          index == highlight ? SPLASH_COLOR_TERTIARY : SPLASH_COLOR_OUTLINE);
    }
}

static void splash_paint_track(const splash_canvas_t *canvas, int filled, uint16_t fill_color)
{
    const splash_shape_t track = {
        .x = SPLASH_TRACK_X,
        .y = SPLASH_TRACK_Y,
        .w = SPLASH_TRACK_W,
        .h = SPLASH_TRACK_H,
        .radius = SPLASH_TRACK_RADIUS,
    };
    splash_fill_shape(canvas, track, SPLASH_COLOR_SURFACE);
    if (filled <= 0) {
        return;
    }
    splash_shape_t progress = track;
    progress.w = filled > SPLASH_TRACK_W ? SPLASH_TRACK_W : filled;
    splash_fill_shape(canvas, progress, fill_color);
}

/** 整幅画面：静态部分与两个动态部分共用同一套绘制，靠 canvas 窗口裁剪区分。 */
static void splash_paint_frame(const splash_canvas_t *canvas, int highlight, int filled,
                               uint16_t fill_color)
{
    splash_clear(canvas);

    const splash_shape_t mark = {
        .x = SPLASH_MARK_X,
        .y = SPLASH_MARK_Y,
        .w = SPLASH_MARK_W,
        .h = SPLASH_MARK_H,
        .radius = SPLASH_MARK_RADIUS,
    };
    splash_fill_shape(canvas, mark, SPLASH_COLOR_SURFACE);
    splash_fill_rect(canvas,
                     SPLASH_DPAD_CX - SPLASH_DPAD_HALF,
                     SPLASH_CONTROLS_CY - SPLASH_DPAD_THICK / 2,
                     SPLASH_DPAD_HALF * 2,
                     SPLASH_DPAD_THICK,
                     SPLASH_COLOR_PRIMARY);
    splash_fill_rect(canvas,
                     SPLASH_DPAD_CX - SPLASH_DPAD_THICK / 2,
                     SPLASH_CONTROLS_CY - SPLASH_DPAD_HALF,
                     SPLASH_DPAD_THICK,
                     SPLASH_DPAD_HALF * 2,
                     SPLASH_COLOR_PRIMARY);
    splash_paint_dots(canvas, highlight);
    splash_paint_track(canvas, filled, fill_color);
}

static splash_canvas_t splash_dots_canvas(void)
{
    const splash_canvas_t canvas = {
        .pixels = s_splash.dots,
        .width = SPLASH_DOTS_W,
        .height = SPLASH_DOTS_H,
        .origin_x = SPLASH_DOTS_X,
        .origin_y = SPLASH_DOTS_Y,
        .base_color = SPLASH_COLOR_SURFACE,
    };
    return canvas;
}

static splash_canvas_t splash_bar_canvas(void)
{
    const splash_canvas_t canvas = {
        .pixels = s_splash.bar,
        .width = SPLASH_TRACK_W,
        .height = SPLASH_TRACK_H,
        .origin_x = SPLASH_TRACK_X,
        .origin_y = SPLASH_TRACK_Y,
        .base_color = SPLASH_COLOR_BACKGROUND,
    };
    return canvas;
}

/** 提交两个动态区域；启动过程的区域更新失败不阻断启动，只记录。 */
static void splash_flush(bool dots_changed)
{
    if (dots_changed) {
        esp_err_t result = panel_transfer(s_splash.dots, SPLASH_DOTS_X, SPLASH_DOTS_Y,
                                          SPLASH_DOTS_W, SPLASH_DOTS_H);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "dots transfer failed: %s", esp_err_to_name(result));
        }
    }
    esp_err_t result = panel_transfer(s_splash.bar, SPLASH_TRACK_X, SPLASH_TRACK_Y,
                                      SPLASH_TRACK_W, SPLASH_TRACK_H);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "progress transfer failed: %s", esp_err_to_name(result));
    }
}

static uint16_t *splash_alloc(size_t pixels)
{
    return heap_caps_aligned_alloc(64, pixels * sizeof(uint16_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/** 重画两个动态区域：内容没变化就不传输，避免启动期无谓的 SPI 流量。 */
static void splash_render(int filled, int highlight, uint16_t fill_color)
{
    const bool dots_changed = highlight != s_splash.highlight;
    const bool bar_changed = filled != s_splash.filled || fill_color != s_splash.fill_color;
    if (!dots_changed && !bar_changed) {
        return;
    }
    if (dots_changed) {
        const splash_canvas_t dots = splash_dots_canvas();
        splash_clear(&dots);
        splash_paint_dots(&dots, highlight);
    }
    if (bar_changed) {
        const splash_canvas_t bar = splash_bar_canvas();
        splash_clear(&bar);
        splash_paint_track(&bar, filled, fill_color);
    }
    s_splash.filled = filled;
    s_splash.highlight = highlight;
    s_splash.fill_color = fill_color;
    splash_flush(dots_changed);
}

/** 一次相位推进：阶段权重给锚点，阶段内按经过时间线性前进，超时停在阶段末。 */
static void splash_anim_step(void)
{
    if (s_splash.failed) {
        const int filled = s_splash.filled > SPLASH_TRACK_H ? s_splash.filled : SPLASH_TRACK_H;
        splash_render(filled, s_splash.highlight, SPLASH_COLOR_ERROR);
        return;
    }
    uint32_t elapsed_ms = 0;
    const int64_t elapsed_us = esp_timer_get_time() - s_splash.stage_started_us;
    if (elapsed_us > 0) {
        elapsed_ms = (uint32_t)(elapsed_us / 1000);
    }
    const uint32_t expected_ms = s_splash.stage_ms[s_splash.stage];
    if (elapsed_ms > expected_ms) {
        elapsed_ms = expected_ms;
    }
    const uint32_t covered_ms = s_splash.stage_base_ms + elapsed_ms;
    const int filled = (int)((uint64_t)SPLASH_TRACK_W * covered_ms / s_splash.total_ms);
    const int highlight =
        (int)((uint64_t)SPLASH_DOT_COUNT * covered_ms / s_splash.total_ms) % SPLASH_DOT_COUNT;
    splash_render(filled, highlight, SPLASH_COLOR_PRIMARY);
}

/** 动画任务：唯一绘制者，退出前释放两块区域缓冲（end 只负责停表）。 */
static void splash_anim_task(void *param)
{
    (void)param;
    while (s_splash.active) {
        splash_anim_step();
        vTaskDelay(pdMS_TO_TICKS(s_splash.failed ? 1000 : SPLASH_ANIM_MS));
    }
    heap_caps_free(s_splash.dots);
    heap_caps_free(s_splash.bar);
    s_splash.dots = NULL;
    s_splash.bar = NULL;
    s_splash.anim_running = false;
    vTaskDelete(NULL);
}

esp_err_t boot_splash_begin(uint8_t brightness_pct, const uint32_t *stage_ms, size_t stage_count)
{
    if (s_splash.active || stage_ms == NULL || stage_count == 0 ||
        stage_count > SPLASH_STAGE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_splash.anim_running) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t frame_pixels = (size_t)SPLASH_WIDTH * SPLASH_HEIGHT;
    uint16_t *frame = splash_alloc(frame_pixels);
    if (frame == NULL) {
        ESP_LOGE(TAG, "splash frame buffer unavailable (%u bytes)",
                 (unsigned)(frame_pixels * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    const splash_canvas_t canvas = {
        .pixels = frame,
        .width = SPLASH_WIDTH,
        .height = SPLASH_HEIGHT,
        .origin_x = 0,
        .origin_y = 0,
        .base_color = SPLASH_COLOR_BACKGROUND,
    };
    splash_paint_frame(&canvas, 0, 0, SPLASH_COLOR_PRIMARY);
    const esp_err_t transfer = panel_transfer(frame, 0, 0, SPLASH_WIDTH, SPLASH_HEIGHT);
    /* 整帧只画一次：两个动态区域有各自的缓冲，继续占着全屏缓冲没有意义。 */
    heap_caps_free(frame);
    if (transfer != ESP_OK) {
        ESP_LOGE(TAG, "splash frame transfer failed: %s", esp_err_to_name(transfer));
        return transfer;
    }

    s_splash.dots = splash_alloc((size_t)SPLASH_DOTS_W * SPLASH_DOTS_H);
    s_splash.bar = splash_alloc((size_t)SPLASH_TRACK_W * SPLASH_TRACK_H);
    if (s_splash.dots == NULL || s_splash.bar == NULL) {
        heap_caps_free(s_splash.dots);
        heap_caps_free(s_splash.bar);
        s_splash.dots = NULL;
        s_splash.bar = NULL;
        ESP_LOGE(TAG, "splash region buffers unavailable");
        return ESP_ERR_NO_MEM;
    }
    uint32_t total_ms = 0;
    for (size_t i = 0; i < stage_count; ++i) {
        s_splash.stage_ms[i] = stage_ms[i];
        total_ms += stage_ms[i];
    }
    s_splash.stage_count = stage_count;
    s_splash.total_ms = total_ms == 0 ? 1 : total_ms;
    s_splash.stage = 0;
    s_splash.stage_base_ms = 0;
    s_splash.stage_started_us = esp_timer_get_time();
    /* -1 让首帧动画无条件重画两个区域（缓冲是未初始化的 PSRAM）。 */
    s_splash.filled = -1;
    s_splash.highlight = -1;
    s_splash.fill_color = SPLASH_COLOR_PRIMARY;
    s_splash.failed = false;
    s_splash.active = true;
    if (xTaskCreate(splash_anim_task, "boot-splash", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "splash animation task unavailable, static splash kept");
    } else {
        s_splash.anim_running = true;
    }

    /* 背光在启动画面落屏之后点亮：提前点亮会先闪出未初始化的面板内容。 */
    const esp_err_t backlight_result = backlight_set(brightness_pct);
    if (backlight_result != ESP_OK) {
        ESP_LOGW(TAG, "backlight on failed: %s", esp_err_to_name(backlight_result));
    }
    ESP_LOGI(TAG, "splash painted (%dx%d), backlight %u%%", SPLASH_WIDTH, SPLASH_HEIGHT,
             (unsigned)brightness_pct);
    return ESP_OK;
}

void boot_splash_progress(int step)
{
    if (!s_splash.active || s_splash.failed) {
        return;
    }
    int index = step - 1;
    if (index < 0) {
        index = 0;
    }
    if (index > (int)s_splash.stage_count - 1) {
        index = (int)s_splash.stage_count - 1;
    }
    uint32_t base_ms = 0;
    for (int i = 0; i < index; ++i) {
        base_ms += s_splash.stage_ms[i];
    }
    s_splash.stage = index;
    s_splash.stage_base_ms = base_ms;
    s_splash.stage_started_us = esp_timer_get_time();
}

void boot_splash_fail(void)
{
    if (!s_splash.active || s_splash.failed) {
        return;
    }
    /* 失败时进度条改错误色并留一个可见的最小长度：停在哪个阶段一眼可见。
     * 画面留在屏上，动画任务继续持有缓冲，等人工复位。 */
    s_splash.failed = true;
    ESP_LOGE(TAG, "boot splash marked failed at stage %d", s_splash.stage + 1);
}

void boot_splash_end(void)
{
    if (!s_splash.active) {
        return;
    }
    s_splash.active = false;
    if (s_splash.anim_running) {
        /* 动画任务是自己退出的那个：等它释放缓冲，最多 300 ms。 */
        for (int waited = 0; waited < 300 && s_splash.anim_running; ++waited) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    if (!s_splash.anim_running) {
        heap_caps_free(s_splash.dots);
        heap_caps_free(s_splash.bar);
        s_splash.dots = NULL;
        s_splash.bar = NULL;
    }
    s_splash.filled = -1;
    s_splash.highlight = -1;
}
