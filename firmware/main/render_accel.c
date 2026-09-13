#include "render_accel.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** 除 255 的整数形式：u / 255 == (u + 1 + (u >> 8)) >> 8，u < 65281。
 * 参考实现写作 (x + 127) / 255，这里先加 127 再走同一条乘加路径，逐像素结果
 * 一致，同时避免 Xtensa 上每次除法都调用 __udivsi3。 */
static inline uint32_t div255_round(uint32_t numerator)
{
    const uint32_t biased = numerator + 127U;
    return (biased + 1U + (biased >> 8)) >> 8;
}

static inline uint16_t pack_rgb565(uint32_t red, uint32_t green, uint32_t blue)
{
    return (uint16_t)(((red & 0xf8U) << 8) | ((green & 0xfcU) << 3) | (blue >> 3));
}

/** 单个像素的 src-over 合成；alpha 已含掩码覆盖率与全局透明度。 */
static inline void blend_pixel(uint16_t *pixel, uint32_t red, uint32_t green,
                               uint32_t blue, uint32_t alpha)
{
    if (alpha == 0U) {
        return;
    }
    if (alpha >= 255U) {
        *pixel = pack_rgb565(red, green, blue);
        return;
    }
    const uint32_t packed = *pixel;
    const uint32_t r5 = (packed >> 11) & 0x1fU;
    const uint32_t g6 = (packed >> 5) & 0x3fU;
    const uint32_t b5 = packed & 0x1fU;
    const uint32_t inverse = 255U - alpha;
    *pixel = pack_rgb565(
        div255_round(red * alpha + ((r5 << 3) | (r5 >> 2)) * inverse),
        div255_round(green * alpha + ((g6 << 2) | (g6 >> 4)) * inverse),
        div255_round(blue * alpha + ((b5 << 3) | (b5 >> 2)) * inverse));
}

/** 目标区间是否落在 width × height 的表面上。 */
static inline bool rect_in_surface(pocketjs_rgb565_rect_t rect, uint32_t width,
                                   uint32_t height)
{
    return rect.width != 0U && rect.height != 0U && rect.x <= width - rect.width &&
           rect.y <= height - rect.height;
}

/** 掩码一行是否全等于 value：先按 4 字节整字比较，尾部再逐字节。圆角矩形与
 * 文字掩码的整行不是全透明就是接近全不透明，整行判定把这两类行从逐像素合成
 * 里摘出去，省掉每像素三次 div255。 */
static bool mask_row_all(const uint8_t *row, uint32_t count, uint8_t value)
{
    const uint32_t wide = (uint32_t)value * 0x01010101U;
    uint32_t index = 0;
    for (; index + 4U <= count; index += 4U) {
        uint32_t chunk = 0;
        memcpy(&chunk, row + index, sizeof(chunk));
        if (chunk != wide) {
            return false;
        }
    }
    for (; index < count; ++index) {
        if (row[index] != value) {
            return false;
        }
    }
    return true;
}

/** 用 source 色整行直写（alpha 已等于 255）。 */
static void fill_row(uint16_t *pixels, uint32_t count, uint16_t color)
{
    for (uint32_t index = 0; index < count; ++index) {
        pixels[index] = color;
    }
}

static bool accel_fill(void *user_data, uint16_t *destination,
                       size_t destination_pixels, uint32_t width,
                       uint32_t height, pocketjs_rgb565_rect_t rect,
                       uint16_t color)
{
    (void)user_data;
    if (destination == NULL || width == 0U || height == 0U ||
        destination_pixels < (size_t)width * height ||
        !rect_in_surface(rect, width, height)) {
        return false;
    }
    for (uint32_t row = 0; row < rect.height; ++row) {
        uint16_t *target = destination + (size_t)(rect.y + row) * width + rect.x;
        for (uint32_t column = 0; column < rect.width; ++column) {
            target[column] = color;
        }
    }
    return true;
}

static bool accel_blend(void *user_data, uint16_t *destination,
                        size_t destination_pixels, uint32_t width,
                        uint32_t height, const uint8_t *mask, size_t mask_size,
                        pocketjs_rgb565_rect_t rect, uint8_t red, uint8_t green,
                        uint8_t blue, uint8_t global_alpha)
{
    (void)user_data;
    if (destination == NULL || mask == NULL || width == 0U || height == 0U ||
        destination_pixels < (size_t)width * height ||
        mask_size < (size_t)width * height ||
        !rect_in_surface(rect, width, height)) {
        return false;
    }
    const uint32_t source_red = red;
    const uint32_t source_green = green;
    const uint32_t source_blue = blue;
    const uint16_t opaque = pack_rgb565(source_red, source_green, source_blue);
    for (uint32_t row = 0; row < rect.height; ++row) {
        const size_t offset = (size_t)(rect.y + row) * width + rect.x;
        uint16_t *pixels = destination + offset;
        const uint8_t *coverage = mask + offset;
        if (global_alpha >= 255U) {
            if (mask_row_all(coverage, rect.width, 0U)) {
                continue;
            }
            if (mask_row_all(coverage, rect.width, 255U)) {
                fill_row(pixels, rect.width, opaque);
                continue;
            }
            for (uint32_t column = 0; column < rect.width; ++column) {
                blend_pixel(&pixels[column], source_red, source_green,
                            source_blue, coverage[column]);
            }
        } else {
            if (mask_row_all(coverage, rect.width, 0U)) {
                continue;
            }
            for (uint32_t column = 0; column < rect.width; ++column) {
                blend_pixel(&pixels[column], source_red, source_green,
                            source_blue,
                            div255_round((uint32_t)coverage[column] *
                                         (uint32_t)global_alpha));
            }
        }
    }
    return true;
}

/** PSM 5650（B5:G6:R5 小端）单色位图的直拷，通道序与 RGB565 相反。 */
static bool accel_srm(void *user_data, uint16_t *destination,
                      size_t destination_pixels, uint32_t width, uint32_t height,
                      const uint8_t *source, size_t source_size,
                      uint32_t source_width, uint32_t source_height,
                      pocketjs_rgb565_rect_t source_rect,
                      pocketjs_rgb565_rect_t destination_rect,
                      uint32_t quarter_turn, bool mirror_x, bool mirror_y)
{
    (void)user_data;
    if (destination == NULL || source == NULL || width == 0U || height == 0U ||
        source_width == 0U || source_height == 0U ||
        destination_pixels < (size_t)width * height ||
        source_size < (size_t)source_width * source_height * 2U ||
        !rect_in_surface(source_rect, source_width, source_height) ||
        !rect_in_surface(destination_rect, width, height)) {
        return false;
    }
    /* 只接管 1:1 直拷；旋转与缩放留给通用软件路径。 */
    if (quarter_turn != 0U || source_rect.width != destination_rect.width ||
        source_rect.height != destination_rect.height) {
        return false;
    }
    for (uint32_t row = 0; row < destination_rect.height; ++row) {
        const uint32_t source_y = mirror_y
                                      ? source_rect.y + source_rect.height - 1U - row
                                      : source_rect.y + row;
        const uint8_t *source_row =
            source + ((size_t)source_y * source_width + source_rect.x) * 2U;
        uint16_t *target_row =
            destination + (size_t)(destination_rect.y + row) * width +
            destination_rect.x;
        for (uint32_t column = 0; column < destination_rect.width; ++column) {
            const uint32_t source_x =
                mirror_x ? destination_rect.width - 1U - column : column;
            const uint32_t psm5650 = (uint32_t)source_row[source_x * 2U] |
                                     ((uint32_t)source_row[source_x * 2U + 1U] << 8);
            target_row[column] = (uint16_t)(((psm5650 & 0x001fU) << 11) |
                                            (psm5650 & 0x07e0U) |
                                            ((psm5650 & 0xf800U) >> 11));
        }
    }
    return true;
}

static const pocketjs_rgb565_accelerator_t s_accelerator = {
    .struct_size = sizeof(pocketjs_rgb565_accelerator_t),
    .user_data = NULL,
    .fill_rgb565 = accel_fill,
    .blend_a8_rgb565 = accel_blend,
    .srm_psm5650_rgb565 = accel_srm,
};

const pocketjs_rgb565_accelerator_t *render_accel(void)
{
    return &s_accelerator;
}
