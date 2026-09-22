/**
 * 本机 damage 差分（render_damage.c）主机端用例：结构变化（切页那种「一段被换掉、
 * 其余原样」的 draw list）只把被换掉的那块交回重画，而不是整屏；scissor 边界、
 * 视口裁剪、字形范围与区域数上限一并钉住。
 */
#include "host_test.h"

#include <string.h>

#include "render_damage.h"

#define SCREEN_W 240u
#define SCREEN_H 280u

/* op 码：与 render_damage.c 内的解码表一致（DrawList v1）。 */
#define OP_RECT 1u
#define OP_GLYPH_RUN 3u
#define OP_SCISSOR 5u
#define OP_SCISSOR_POP 6u

static uint32_t xy_word(int x, int y)
{
    return (uint32_t)(uint16_t)x | ((uint32_t)(uint16_t)y << 16);
}

static uint32_t wh_word(unsigned width, unsigned height)
{
    return width | (height << 16);
}

/** 追加一个填充矩形 op。 */
static size_t push_rect(uint32_t *words, size_t at, int x, int y, unsigned width,
                        unsigned height, uint32_t color)
{
    words[at + 0U] = OP_RECT;
    words[at + 1U] = xy_word(x, y);
    words[at + 2U] = wh_word(width, height);
    words[at + 3U] = color;
    return at + 4U;
}

static size_t push_scissor(uint32_t *words, size_t at, int x, int y, unsigned width,
                           unsigned height)
{
    words[at + 0U] = OP_SCISSOR;
    words[at + 1U] = xy_word(x, y);
    words[at + 2U] = wh_word(width, height);
    return at + 3U;
}

static bool diff(const uint32_t *previous, size_t previous_words, const uint32_t *current,
                 size_t current_words, render_damage_plan_t *plan)
{
    return render_damage_diff(previous, previous_words, current, current_words, SCREEN_W,
                              SCREEN_H, NULL, NULL, plan);
}

static bool plan_contains(const render_damage_plan_t *plan, int x, int y, unsigned width,
                          unsigned height)
{
    for (uint32_t index = 0U; index < plan->count; ++index) {
        const render_damage_rect_t *rect = &plan->rects[index];
        if ((int32_t)rect->x <= x && (int32_t)rect->y <= y &&
            (int32_t)(rect->x + rect->width) >= x + (int32_t)width &&
            (int32_t)(rect->y + rect->height) >= y + (int32_t)height) {
            return true;
        }
    }
    return false;
}

/** 测试用图集查询：user_data 指向 {字格宽, 字格高, 字形数}。 */
static bool test_font_lookup(void *user_data, uint32_t slot, uint32_t *cell_width,
                             uint32_t *cell_height, uint32_t *glyph_count)
{
    (void)slot;
    const uint32_t *cell = user_data;
    *cell_width = cell[0];
    *cell_height = cell[1];
    *glyph_count = cell[2];
    return true;
}

/** 一页换成另一页：首尾一致、中间一段被替换，只有被换掉的那块该重画。 */
static void structural_swap_keeps_damage_inside_the_replaced_block(void)
{
    uint32_t previous[512];
    uint32_t current[512];
    size_t previous_at = 0U;
    size_t current_at = 0U;
    for (int index = 0; index < 8; ++index) {
        previous_at = push_rect(previous, previous_at, index * 20, 0, 16, 16, 0xff000000U);
        current_at = push_rect(current, current_at, index * 20, 0, 16, 16, 0xff000000U);
    }
    /* 中段：旧的三条内容换成另外三条（位置与颜色都不同），等价于切页时页面内容被换掉。 */
    for (int index = 0; index < 3; ++index) {
        previous_at = push_rect(previous, previous_at, 54 + index * 30, 56, 24, 24, 0xff101010U);
        current_at = push_rect(current, current_at, 54 + index * 30, 56, 20, 20, 0xff202020U);
    }
    for (int index = 0; index < 4; ++index) {
        previous_at = push_rect(previous, previous_at, 0, 200 + index * 12, 224, 8, 0xff303030U);
        current_at = push_rect(current, current_at, 0, 200 + index * 12, 224, 8, 0xff303030U);
    }

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_at, current, current_at, &plan));
    CHECK(plan.count >= 1u);
    for (int index = 0; index < 3; ++index) {
        CHECK(plan_contains(&plan, 54 + index * 30, 56, 24, 24));
    }
    /* 屏幕其余部分（底部状态栏那一带）不进 damage，区域总量也远小于整屏。 */
    uint64_t damaged = 0U;
    for (uint32_t index = 0U; index < plan.count; ++index) {
        CHECK(plan.rects[index].y < 100u);
        damaged += (uint64_t)plan.rects[index].width * (uint64_t)plan.rects[index].height;
    }
    CHECK(damaged < 4096U);
}

static void identical_draw_lists_need_no_repaint(void)
{
    uint32_t words[64];
    size_t at = push_rect(words, 0U, 4, 4, 32, 16, 0xff123456U);
    at = push_rect(words, at, 40, 40, 8, 8, 0xffabcdefU);

    render_damage_plan_t plan;
    REQUIRE(diff(words, at, words, at, &plan));
    CHECK_EQ(plan.count, 0u);
}

static void recolored_rect_repaints_only_its_own_box(void)
{
    uint32_t previous[8];
    uint32_t current[8];
    const size_t previous_words = push_rect(previous, 0U, 10, 20, 30, 40, 0xff000000U);
    const size_t current_words = push_rect(current, 0U, 10, 20, 30, 40, 0xff112233U);

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].x, 10u);
    CHECK_EQ(plan.rects[0].y, 20u);
    CHECK_EQ(plan.rects[0].width, 30u);
    CHECK_EQ(plan.rects[0].height, 40u);
}

static void added_block_repaints_just_that_block(void)
{
    uint32_t previous[64];
    uint32_t current[64];
    size_t previous_words = push_rect(previous, 0U, 0, 0, 16, 16, 0xff000000U);
    previous_words = push_rect(previous, previous_words, 40, 0, 16, 16, 0xff000000U);
    size_t current_words = push_rect(current, 0U, 0, 0, 16, 16, 0xff000000U);
    current_words = push_rect(current, current_words, 20, 0, 8, 8, 0xff00ff00U);
    current_words = push_rect(current, current_words, 40, 0, 16, 16, 0xff000000U);

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].x, 20u);
    CHECK_EQ(plan.rects[0].width, 8u);
    CHECK_EQ(plan.rects[0].height, 8u);
}

static void removed_block_repaints_just_that_block(void)
{
    uint32_t previous[64];
    uint32_t current[64];
    size_t previous_words = push_rect(previous, 0U, 0, 0, 16, 16, 0xff000000U);
    previous_words = push_rect(previous, previous_words, 20, 4, 8, 8, 0xff00ff00U);
    previous_words = push_rect(previous, previous_words, 40, 0, 16, 16, 0xff000000U);
    size_t current_words = push_rect(current, 0U, 0, 0, 16, 16, 0xff000000U);
    current_words = push_rect(current, current_words, 40, 0, 16, 16, 0xff000000U);

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].x, 20u);
    CHECK_EQ(plan.rects[0].y, 4u);
}

static void scissor_crops_damage_to_the_visible_part(void)
{
    uint32_t previous[64];
    uint32_t current[64];
    size_t previous_words = push_scissor(previous, 0U, 0, 0, 32, 32);
    previous_words = push_rect(previous, previous_words, 0, 0, 100, 10, 0xff000000U);
    previous[previous_words] = OP_SCISSOR_POP;
    previous_words += 1U;
    size_t current_words = push_scissor(current, 0U, 0, 0, 32, 32);
    current_words = push_rect(current, current_words, 0, 0, 100, 10, 0xff0f0f0fU);
    current[current_words] = OP_SCISSOR_POP;
    current_words += 1U;

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].width, 32u);
    CHECK_EQ(plan.rects[0].height, 10u);
}

static void disjoint_changes_keep_separate_regions(void)
{
    uint32_t previous[16];
    uint32_t current[16];
    size_t previous_words = push_rect(previous, 0U, 0, 0, 20, 20, 0xff000000U);
    previous_words = push_rect(previous, previous_words, 100, 0, 20, 20, 0xff000000U);
    size_t current_words = push_rect(current, 0U, 0, 0, 20, 20, 0xff111111U);
    current_words = push_rect(current, current_words, 100, 0, 20, 20, 0xff222222U);

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    CHECK_EQ(plan.count, 2u);
}

static void off_screen_boxes_are_clipped_to_the_viewport(void)
{
    uint32_t previous[8];
    uint32_t current[8];
    const size_t previous_words = push_rect(previous, 0U, -10, -10, 20, 20, 0xff000000U);
    const size_t current_words = push_rect(current, 0U, -10, -10, 20, 20, 0xff0f0f0fU);

    render_damage_plan_t plan;
    REQUIRE(diff(previous, previous_words, current, current_words, &plan));
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].x, 0u);
    CHECK_EQ(plan.rects[0].y, 0u);
    CHECK_EQ(plan.rects[0].width, 10u);
    CHECK_EQ(plan.rects[0].height, 10u);
}

static void glyph_run_uses_the_atlas_cell_box(void)
{
    uint32_t previous[32];
    uint32_t current[32];
    static const uint32_t cell[3] = {8u, 8u, 4u};
    for (size_t frame = 0U; frame < 2U; ++frame) {
        uint32_t *words = frame == 0U ? previous : current;
        words[0] = OP_GLYPH_RUN;
        words[1] = 0U | (2U << 16); /* slot 0，两个字 */
        words[2] = frame == 0U ? 0xff000000U : 0xff336699U;
        words[3] = xy_word(10, 20);
        words[4] = 0U;
        words[5] = xy_word(30, 40);
        words[6] = 3U;
    }

    render_damage_plan_t plan;
    const bool ok = render_damage_diff(previous, 7U, current, 7U, SCREEN_W, SCREEN_H,
                                       test_font_lookup, (void *)cell, &plan);
    REQUIRE(ok);
    REQUIRE(plan.count == 1u);
    CHECK_EQ(plan.rects[0].x, 10u);
    CHECK_EQ(plan.rects[0].y, 20u);
    CHECK_EQ(plan.rects[0].width, 28u);
    CHECK_EQ(plan.rects[0].height, 28u);
}

static void many_disjoint_changes_stay_within_the_region_cap(void)
{
    enum { CHANGE_COUNT = 12 };
    uint32_t previous[128];
    uint32_t current[128];
    size_t at = 0U;
    for (int index = 0; index < CHANGE_COUNT; ++index) {
        at = push_rect(previous, at, index * 18, 0, 8, 8, 0xff000000U);
    }
    size_t current_at = 0U;
    for (int index = 0; index < CHANGE_COUNT; ++index) {
        current_at = push_rect(current, current_at, index * 18, 0, 8, 8, 0xff0f0f0fu);
    }

    render_damage_plan_t plan;
    REQUIRE(diff(previous, at, current, current_at, &plan));
    CHECK(plan.count <= RENDER_DAMAGE_MAX_REGIONS);
    for (int index = 0; index < CHANGE_COUNT; ++index) {
        CHECK(plan_contains(&plan, index * 18, 0, 8, 8));
    }
}

static void unknown_op_code_falls_back_to_full_redraw(void)
{
    uint32_t previous[16];
    uint32_t current[16];
    previous[0] = 99U;
    previous[1] = 1U;
    previous[2] = 2U;
    previous[3] = 3U;
    memcpy(current, previous, sizeof(previous));
    current[1] = 7U;

    render_damage_plan_t plan;
    CHECK(!diff(previous, 4U, current, 4U, &plan));
}

static void unbalanced_scissor_falls_back_to_full_redraw(void)
{
    uint32_t previous[16];
    uint32_t current[16];
    size_t previous_words = push_scissor(previous, 0U, 0, 0, 32, 32);
    previous_words = push_rect(previous, previous_words, 0, 0, 8, 8, 0xff000000U);
    previous[previous_words] = OP_SCISSOR_POP;
    previous_words += 1U;
    size_t current_words = push_scissor(current, 0U, 0, 0, 32, 32);
    current_words = push_rect(current, current_words, 0, 0, 8, 8, 0xff0f0f0fU);

    render_damage_plan_t plan;
    CHECK(!diff(previous, previous_words, current, current_words, &plan));
}

static void empty_streams_fall_back_to_full_redraw(void)
{
    uint32_t words[4];
    const size_t at = push_rect(words, 0U, 0, 0, 8, 8, 0xff000000U);
    render_damage_plan_t plan;
    CHECK(!diff(words, 0U, words, at, &plan));
    CHECK(!diff(words, at, words, 0U, &plan));
}

HOST_TEST_SUITE(suite_render_damage, "render_damage 本机 damage 差分",
                {"切页那种一段被换掉只重画那一块，其余部分不动",
                 structural_swap_keeps_damage_inside_the_replaced_block},
                {"两帧逐字一致时没有待重画区域", identical_draw_lists_need_no_repaint},
                {"只有颜色变化的矩形按自身方框重画", recolored_rect_repaints_only_its_own_box},
                {"新增的绘制块只重画新增块", added_block_repaints_just_that_block},
                {"移除的绘制块只重画移除块", removed_block_repaints_just_that_block},
                {"scissor 内的变化不超过 scissor 边界",
                 scissor_crops_damage_to_the_visible_part},
                {"不相接的两块变化各自成区域", disjoint_changes_keep_separate_regions},
                {"视口外的范围裁到视口内", off_screen_boxes_are_clipped_to_the_viewport},
                {"字形运行按图集字格取范围", glyph_run_uses_the_atlas_cell_box},
                {"变化再多区域数也不超过上限", many_disjoint_changes_stay_within_the_region_cap},
                {"认不出的 op 码回退整屏", unknown_op_code_falls_back_to_full_redraw},
                {"scissor 不配对回退整屏", unbalanced_scissor_falls_back_to_full_redraw},
                {"空 draw list 回退整屏", empty_streams_fall_back_to_full_redraw});
