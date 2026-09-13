/**
 * 本机像素加速回调（render_accel.c）：官方渲染器把填充、A8 掩码混合与
 * PSM5650 直拷交给这组回调，结果必须与软件路径逐像素一致——差一位就是
 * 屏幕上的色带或错行。主机端直接按像素比对，比在真机上盯画面快得多。
 *
 * 调用参数顺序见 pocketjs/render_types.h 的三个函数指针类型。
 */
#include "host_test.h"

#include <string.h>

#include "render_accel.h"

#define SURFACE_W 16u
#define SURFACE_H 8u
#define SURFACE_PIXELS (SURFACE_W * SURFACE_H)

/* 直拷用例的源表面尺寸：数组长度要用编译期常量，不能用 const 变量。 */
#define SMALL_W 4u
#define SMALL_H 2u
#define SMALL_PIXELS (SMALL_W * SMALL_H)

static pocketjs_rgb565_rect_t rect_of(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    pocketjs_rgb565_rect_t rect = {x, y, width, height};
    return rect;
}

/** 参考实现：与加速回调同一套取整规则（先加 127 再除以 255）。 */
static uint16_t pack_rgb565(uint32_t red, uint32_t green, uint32_t blue)
{
    return (uint16_t)(((red & 0xf8u) << 8) | ((green & 0xfcu) << 3) | (blue >> 3));
}

static uint32_t expand_r5(uint32_t value)
{
    return (value << 3) | (value >> 2);
}

static uint32_t expand_g6(uint32_t value)
{
    return (value << 2) | (value >> 4);
}

static uint16_t blend_reference(uint16_t background, uint8_t red, uint8_t green, uint8_t blue,
                                 uint32_t alpha)
{
    if (alpha == 0u) {
        return background;
    }
    if (alpha >= 255u) {
        return pack_rgb565(red, green, blue);
    }
    const uint32_t inverse = 255u - alpha;
    const uint32_t r5 = (background >> 11) & 0x1fu;
    const uint32_t g6 = (background >> 5) & 0x3fu;
    const uint32_t b5 = background & 0x1fu;
    return pack_rgb565((red * alpha + expand_r5(r5) * inverse + 127u) / 255u,
                       (green * alpha + expand_g6(g6) * inverse + 127u) / 255u,
                       (blue * alpha + expand_r5(b5) * inverse + 127u) / 255u);
}

/** 铺开覆盖全色域的底色：跨字节边界的值最容易暴露取整错误。 */
static uint16_t sample_background(size_t index)
{
    return (uint16_t)((index * 2654435761u) >> 16);
}

/** PSM5650 单色位图的一个像素：小端两字节。 */
static void write_psm5650(uint8_t *source, uint32_t index, uint16_t word)
{
    source[index * 2] = (uint8_t)(word & 0xFF);
    source[index * 2 + 1] = (uint8_t)(word >> 8);
}

static uint16_t srm_reference(uint16_t word)
{
    return (uint16_t)(((word & 0x001Fu) << 11) | (word & 0x07E0u) | ((word & 0xF800u) >> 11));
}

static void accelerator_header(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    REQUIRE(accel != NULL);
    CHECK_EQ(accel->struct_size, sizeof(pocketjs_rgb565_accelerator_t));
    CHECK(accel->fill_rgb565 != NULL);
    CHECK(accel->blend_a8_rgb565 != NULL);
    CHECK(accel->srm_psm5650_rgb565 != NULL);
}

static void fill_only_touches_target_rect(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint16_t surface[SURFACE_PIXELS];
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        surface[index] = 0x1111u;
    }

    REQUIRE(accel->fill_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H,
                               rect_of(2, 1, 3, 2), 0xABCDu));
    for (uint32_t y = 0; y < SURFACE_H; y++) {
        for (uint32_t x = 0; x < SURFACE_W; x++) {
            const bool inside = x >= 2 && x < 5 && y >= 1 && y < 3;
            CHECK_EQ(surface[y * SURFACE_W + x], inside ? 0xABCDu : 0x1111u);
        }
    }
}

static void fill_rejects_invalid_arguments(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint16_t surface[4] = {1, 2, 3, 4};

    CHECK(!accel->fill_rgb565(NULL, NULL, 4, 2, 2, rect_of(0, 0, 1, 1), 0));
    CHECK(!accel->fill_rgb565(NULL, surface, 4, 0, 2, rect_of(0, 0, 1, 1), 0));
    CHECK(!accel->fill_rgb565(NULL, surface, 4, 2, 2, rect_of(0, 0, 0, 1), 0));
    /* 目标缓冲小于 width × height：交回软件路径，不能越界写。 */
    CHECK(!accel->fill_rgb565(NULL, surface, 3, 2, 2, rect_of(0, 0, 1, 1), 0));
    /* 矩形越出表面。 */
    CHECK(!accel->fill_rgb565(NULL, surface, 4, 2, 2, rect_of(1, 1, 2, 2), 0));
}

static void blend_alpha_extremes(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint16_t surface[SURFACE_PIXELS];
    uint8_t mask[SURFACE_PIXELS];
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        surface[index] = 0x1234u;
        mask[index] = 0u;
    }

    /* 掩码全 0：整块不动（同时覆盖整行跳过的快路径）。 */
    REQUIRE(accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                   sizeof(mask), rect_of(0, 0, SURFACE_W, SURFACE_H), 255, 0, 0,
                                   255));
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        CHECK_EQ(surface[index], 0x1234u);
    }

    /* 掩码全 255、全局不透明：整块写成源色。 */
    memset(mask, 255, sizeof(mask));
    REQUIRE(accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                   sizeof(mask), rect_of(0, 0, SURFACE_W, SURFACE_H), 0x12, 0x9C,
                                   0xF0, 255));
    const uint16_t opaque = pack_rgb565(0x12, 0x9C, 0xF0);
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        CHECK_EQ(surface[index], opaque);
    }
}

static void blend_matches_reference_over_full_range(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint16_t surface[SURFACE_PIXELS];
    uint16_t reference[SURFACE_PIXELS];
    uint8_t mask[SURFACE_PIXELS];
    static const uint8_t SOURCE_RED = 0xD4;
    static const uint8_t SOURCE_GREEN = 0x2B;
    static const uint8_t SOURCE_BLUE = 0x77;

    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        surface[index] = sample_background(index);
        mask[index] = (uint8_t)(index * 255u / (SURFACE_PIXELS - 1u));
    }
    memcpy(reference, surface, sizeof(surface));

    REQUIRE(accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                   sizeof(mask), rect_of(0, 0, SURFACE_W, SURFACE_H),
                                   SOURCE_RED, SOURCE_GREEN, SOURCE_BLUE, 255));
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        const uint16_t want = blend_reference(reference[index], SOURCE_RED, SOURCE_GREEN,
                                              SOURCE_BLUE, mask[index]);
        CHECK_EQ(surface[index], want);
    }

    /* 全局透明度再乘一层：有效 alpha 走同一条取整规则。 */
    memcpy(surface, reference, sizeof(surface));
    REQUIRE(accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                   sizeof(mask), rect_of(0, 0, SURFACE_W, SURFACE_H),
                                   SOURCE_RED, SOURCE_GREEN, SOURCE_BLUE, 128));
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        const uint32_t scaled = ((uint32_t)mask[index] * 128u + 127u) / 255u;
        const uint16_t want = blend_reference(reference[index], SOURCE_RED, SOURCE_GREEN,
                                              SOURCE_BLUE, scaled);
        CHECK_EQ(surface[index], want);
    }
}

static void blend_only_writes_inside_rect(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint16_t surface[SURFACE_PIXELS];
    uint8_t mask[SURFACE_PIXELS];
    for (size_t index = 0; index < SURFACE_PIXELS; index++) {
        surface[index] = 0x0F0Fu;
        mask[index] = 255u;
    }

    REQUIRE(accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                   sizeof(mask), rect_of(1, 1, 2, 3), 0xFF, 0xFF, 0xFF, 255));
    for (uint32_t y = 0; y < SURFACE_H; y++) {
        for (uint32_t x = 0; x < SURFACE_W; x++) {
            const bool inside = x >= 1 && x < 3 && y >= 1 && y < 4;
            CHECK_EQ(surface[y * SURFACE_W + x], inside ? 0xFFFFu : 0x0F0Fu);
        }
    }

    /* 掩码缓冲不够 width × height 时交回软件路径。 */
    CHECK(!accel->blend_a8_rgb565(NULL, surface, SURFACE_PIXELS, SURFACE_W, SURFACE_H, mask,
                                  SURFACE_PIXELS - 1u, rect_of(0, 0, 2, 2), 0, 0, 0, 255));
}

static void srm_swaps_channels(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint8_t source[SMALL_PIXELS * 2];
    uint16_t destination[SMALL_PIXELS];
    static const uint16_t samples[SMALL_PIXELS] = {0xF800u, 0x07E0u, 0x001Fu, 0x1234u,
                                                   0xABCDu, 0x0000u, 0xFFFFu, 0x0842u};
    for (uint32_t index = 0; index < SMALL_PIXELS; index++) {
        write_psm5650(source, index, samples[index]);
        destination[index] = 0xDEADu;
    }

    REQUIRE(accel->srm_psm5650_rgb565(NULL, destination, SMALL_PIXELS, SMALL_W, SMALL_H, source,
                                      sizeof(source), SMALL_W, SMALL_H,
                                      rect_of(0, 0, SMALL_W, SMALL_H),
                                      rect_of(0, 0, SMALL_W, SMALL_H), 0, false, false));
    for (uint32_t index = 0; index < SMALL_PIXELS; index++) {
        CHECK_EQ(destination[index], srm_reference(samples[index]));
    }
}

static void srm_mirror_and_subrect(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint8_t source[SMALL_PIXELS * 2];
    uint16_t destination[SMALL_PIXELS];
    for (uint32_t index = 0; index < SMALL_PIXELS; index++) {
        write_psm5650(source, index, (uint16_t)(0x0100u * (index + 1u)));
    }
    memset(destination, 0, sizeof(destination));

    /* 取源表面右下 2×1，同时水平与垂直镜像后落回目标左上角。 */
    REQUIRE(accel->srm_psm5650_rgb565(NULL, destination, SMALL_PIXELS, SMALL_W, SMALL_H, source,
                                      sizeof(source), SMALL_W, SMALL_H, rect_of(1, 1, 2, 1),
                                      rect_of(0, 0, 2, 1), 0, true, true));
    /* 源第 1 行第 2、3 列的取值是 0x0600 与 0x0700，镜像后顺序颠倒。 */
    CHECK_EQ(destination[0], srm_reference(0x0700u));
    CHECK_EQ(destination[1], srm_reference(0x0600u));
    CHECK_EQ(destination[2], 0x0000u); /* 矩形只覆盖第一行的两个像素 */
}

static void srm_rejects_transform_and_oversize(void)
{
    const pocketjs_rgb565_accelerator_t *accel = render_accel();
    uint8_t source[SMALL_PIXELS * 2];
    uint16_t destination[SMALL_PIXELS];
    memset(source, 0x11, sizeof(source));
    memset(destination, 0, sizeof(destination));

    /* 旋转与缩放交给通用软件路径。 */
    CHECK(!accel->srm_psm5650_rgb565(NULL, destination, SMALL_PIXELS, SMALL_W, SMALL_H, source,
                                     sizeof(source), SMALL_W, SMALL_H,
                                     rect_of(0, 0, SMALL_W, SMALL_H),
                                     rect_of(0, 0, SMALL_W, SMALL_H), 1, false, false));
    CHECK(!accel->srm_psm5650_rgb565(NULL, destination, SMALL_PIXELS, SMALL_W, SMALL_H, source,
                                     sizeof(source), SMALL_W, SMALL_H,
                                     rect_of(0, 0, SMALL_W, SMALL_H), rect_of(0, 0, 1, 2), 0,
                                     false, false));
    /* 源缓冲不足。 */
    CHECK(!accel->srm_psm5650_rgb565(NULL, destination, SMALL_PIXELS, SMALL_W, SMALL_H, source,
                                     sizeof(source) - 1u, SMALL_W, SMALL_H,
                                     rect_of(0, 0, SMALL_W, SMALL_H),
                                     rect_of(0, 0, SMALL_W, SMALL_H), 0, false, false));
}

HOST_TEST_SUITE(suite_render_accel, "render_accel",
                {"回调表完整", accelerator_header},
                {"填充只动目标矩形", fill_only_touches_target_rect},
                {"填充拒绝非法参数", fill_rejects_invalid_arguments},
                {"掩码混合的 0 / 255 两端", blend_alpha_extremes},
                {"掩码混合逐像素对齐参考实现", blend_matches_reference_over_full_range},
                {"掩码混合只写矩形内", blend_only_writes_inside_rect},
                {"PSM5650 直拷交换通道序", srm_swaps_channels},
                {"PSM5650 直拷支持镜像与子矩形", srm_mirror_and_subrect},
                {"PSM5650 直拷拒绝旋转缩放与越界", srm_rejects_transform_and_oversize});

