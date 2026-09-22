#include "render_damage.h"

#include <string.h>

/* DrawList v1 的 op 码与字长（含头部字），变长 op 按头部计数；范围取法逐条对照
 * PocketJS 引擎的 damage 解码器（engine/core/src/damage.rs）：矩形类取 op 自己的
 * xy/wh 再与当前 scissor 相交，原生文本运行按 scissor 取上界。 */

enum {
    RENDER_OP_RECT = 1,
    RENDER_OP_GRAD_RECT = 2,
    RENDER_OP_GLYPH_RUN = 3,
    RENDER_OP_TEX_QUAD = 4,
    RENDER_OP_SCISSOR = 5,
    RENDER_OP_SCISSOR_POP = 6,
    RENDER_OP_TRI = 7,
    RENDER_OP_TEX_TRI = 8,
    RENDER_OP_TEXT_RUN = 9,
    RENDER_OP_SURFACE_QUAD = 10,
};

/** scissor 栈深：与框架的 CLIP_DEPTH 同量级，越界即判解码失败。 */
#define RENDER_CLIP_DEPTH 8
/** 重同步窗口：结构变化时两侧各自最多跨过这么多 op 找落点。切页那种「一整段
 *  内容被换掉」的落差在数十个 op，窗口必须盖得住整段落差才找得回落点。 */
#define RENDER_RESYNC_WINDOW 96
/** 落点前导匹配长度：连续这么多个 op 码相同才进入逐字校验，用来廉价筛掉
 *  同码不同字的候选对（候选对数量是窗口的平方）。 */
#define RENDER_RESYNC_RUN 3
typedef struct {
    int32_t x0;
    int32_t y0;
    int32_t x1;
    int32_t y1;
} render_rect_t;

typedef struct {
    const uint32_t *words;
    size_t count;
    size_t index;
    render_rect_t screen;
    render_rect_t clip;
    render_rect_t stack[RENDER_CLIP_DEPTH];
    size_t depth;
} render_decoder_t;

typedef struct {
    uint32_t code;
    const uint32_t *words;
    size_t length;
    render_rect_t bounds;
} render_op_t;

typedef enum {
    RENDER_DECODE_END,
    RENDER_DECODE_OP,
    RENDER_DECODE_ERROR,
} render_decode_result_t;

static render_rect_t rect_empty(void)
{
    const render_rect_t rect = {0, 0, 0, 0};
    return rect;
}

static bool rect_is_empty(render_rect_t rect)
{
    return rect.x1 <= rect.x0 || rect.y1 <= rect.y0;
}

static int32_t min_i32(int32_t left, int32_t right)
{
    return left < right ? left : right;
}

static int32_t max_i32(int32_t left, int32_t right)
{
    return left > right ? left : right;
}

static render_rect_t rect_intersect(render_rect_t left, render_rect_t right)
{
    render_rect_t out = {max_i32(left.x0, right.x0), max_i32(left.y0, right.y0),
                         min_i32(left.x1, right.x1), min_i32(left.y1, right.y1)};
    if (rect_is_empty(out)) {
        return rect_empty();
    }
    return out;
}

static render_rect_t rect_union(render_rect_t left, render_rect_t right)
{
    if (rect_is_empty(left)) {
        return right;
    }
    if (rect_is_empty(right)) {
        return left;
    }
    const render_rect_t out = {min_i32(left.x0, right.x0), min_i32(left.y0, right.y0),
                               max_i32(left.x1, right.x1), max_i32(left.y1, right.y1)};
    return out;
}

/** 相接或相交判定，取框架的同一判据（任一侧的边重合即算相接）。 */
static bool rect_touches(render_rect_t left, render_rect_t right)
{
    return left.x0 <= right.x1 && right.x0 <= left.x1 && left.y0 <= right.y1 &&
           right.y0 <= left.y1;
}

static uint64_t rect_area(render_rect_t rect)
{
    if (rect_is_empty(rect)) {
        return 0U;
    }
    return (uint64_t)(rect.x1 - rect.x0) * (uint64_t)(rect.y1 - rect.y0);
}

/** i16 打包坐标：低 16 位 x、高 16 位 y。 */
static int32_t xy_x(uint32_t word)
{
    return (int32_t)(int16_t)(word & 0xffffU);
}

static int32_t xy_y(uint32_t word)
{
    return (int32_t)(int16_t)(word >> 16);
}

/** 无符号打包尺寸：低 16 位宽、高 16 位高。 */
static int32_t wh_w(uint32_t word)
{
    return (int32_t)(word & 0xffffU);
}

static int32_t wh_h(uint32_t word)
{
    return (int32_t)(word >> 16);
}

static render_rect_t logical_rect(uint32_t xy_word, uint32_t wh_word)
{
    const int32_t x = xy_x(xy_word);
    const int32_t y = xy_y(xy_word);
    const int32_t width = wh_w(wh_word);
    const int32_t height = wh_h(wh_word);
    const render_rect_t rect = {x, y, x + width, y + height};
    return rect;
}

/** 三个顶点的外接矩形与 scissor 相交。 */
static render_rect_t triangle_bounds(uint32_t a, uint32_t b, uint32_t c, render_rect_t clip)
{
    const int32_t x0 = min_i32(min_i32(xy_x(a), xy_x(b)), xy_x(c));
    const int32_t x1 = max_i32(max_i32(xy_x(a), xy_x(b)), xy_x(c));
    const int32_t y0 = min_i32(min_i32(xy_y(a), xy_y(b)), xy_y(c));
    const int32_t y1 = max_i32(max_i32(xy_y(a), xy_y(b)), xy_y(c));
    const render_rect_t rect = {x0, y0, x1, y1};
    return rect_intersect(rect, clip);
}

static bool rect_same(render_rect_t left, render_rect_t right)
{
    return left.x0 == right.x0 && left.y0 == right.y0 && left.x1 == right.x1 &&
           left.y1 == right.y1;
}

size_t render_damage_op_length(const uint32_t *words, size_t count, size_t at)
{
    const uint32_t code = words[at];
    switch (code) {
    case RENDER_OP_RECT:
        return 4U;
    case RENDER_OP_GRAD_RECT:
        return 6U;
    case RENDER_OP_TEX_QUAD:
    case RENDER_OP_SURFACE_QUAD:
        return 9U;
    case RENDER_OP_SCISSOR:
        return 3U;
    case RENDER_OP_SCISSOR_POP:
        return 1U;
    case RENDER_OP_TRI:
        return 7U;
    case RENDER_OP_TEX_TRI:
        return 12U;
    case RENDER_OP_GLYPH_RUN: {
        if (at + 3U > count) {
            return 0U;
        }
        const size_t glyphs = (size_t)(words[at + 1U] >> 16);
        if (glyphs > (count - at - 3U) / 2U) {
            return 0U;
        }
        return 3U + glyphs * 2U;
    }
    case RENDER_OP_TEXT_RUN: {
        if (at + 8U > count) {
            return 0U;
        }
        const size_t bytes = (size_t)words[at + 7U];
        if (bytes > (count - at - 8U) * 4U) {
            return 0U;
        }
        return 8U + (bytes + 3U) / 4U;
    }
    default:
        return 0U;
    }
}

/** 内部沿用短名调用。 */
static size_t op_word_length(const uint32_t *words, size_t count, size_t at)
{
    return render_damage_op_length(words, count, at);
}

/** 字形运行的范围：逐字格外接矩形，与框架取法一致；图集未知时退化成整块 scissor。 */
static render_rect_t glyph_run_bounds(const uint32_t *words, render_rect_t clip,
                                      render_damage_font_fn font_lookup, void *font_user_data)
{
    if ((words[2] >> 24) == 0U) {
        return rect_empty();
    }
    uint32_t cell_width = 0U;
    uint32_t cell_height = 0U;
    uint32_t glyph_count = 0U;
    const uint32_t slot = words[1] & 0xffU;
    if (font_lookup == NULL ||
        !font_lookup(font_user_data, slot, &cell_width, &cell_height, &glyph_count)) {
        return clip;
    }
    const size_t glyphs = (size_t)(words[1] >> 16);
    render_rect_t bounds = rect_empty();
    for (size_t index = 0; index < glyphs; ++index) {
        const uint32_t xy = words[3U + index * 2U];
        const uint32_t gid = words[4U + index * 2U] & 0xffffU;
        if (gid >= glyph_count) {
            continue;
        }
        const int32_t x = xy_x(xy);
        const int32_t y = xy_y(xy);
        const render_rect_t cell = {x, y, x + (int32_t)cell_width, y + (int32_t)cell_height};
        bounds = rect_union(bounds, cell);
    }
    return rect_intersect(bounds, clip);
}

static render_decode_result_t decoder_next(render_decoder_t *decoder,
                                           render_damage_font_fn font_lookup,
                                           void *font_user_data, render_op_t *out_op)
{
    if (decoder->index == decoder->count) {
        return RENDER_DECODE_END;
    }
    if (decoder->index > decoder->count) {
        return RENDER_DECODE_ERROR;
    }
    const size_t length = op_word_length(decoder->words, decoder->count, decoder->index);
    if (length == 0U || length > decoder->count - decoder->index) {
        return RENDER_DECODE_ERROR;
    }
    const uint32_t *words = decoder->words + decoder->index;
    const uint32_t code = decoder->words[decoder->index];
    render_rect_t bounds = rect_empty();
    switch (code) {
    case RENDER_OP_RECT:
    case RENDER_OP_GRAD_RECT:
        bounds = rect_intersect(logical_rect(words[1], words[2]), decoder->clip);
        break;
    case RENDER_OP_TEX_QUAD:
        bounds = rect_intersect(logical_rect(words[2], words[3]), decoder->clip);
        break;
    case RENDER_OP_GLYPH_RUN:
        bounds = glyph_run_bounds(words, decoder->clip, font_lookup, font_user_data);
        break;
    case RENDER_OP_TEXT_RUN:
        bounds = decoder->clip;
        break;
    case RENDER_OP_SCISSOR:
        if (decoder->depth >= RENDER_CLIP_DEPTH) {
            return RENDER_DECODE_ERROR;
        }
        decoder->stack[decoder->depth] = decoder->clip;
        decoder->depth += 1U;
        decoder->clip = rect_intersect(decoder->screen, logical_rect(words[1], words[2]));
        bounds = decoder->clip;
        break;
    case RENDER_OP_SCISSOR_POP:
        if (decoder->depth == 0U) {
            return RENDER_DECODE_ERROR;
        }
        decoder->depth -= 1U;
        decoder->clip = decoder->stack[decoder->depth];
        break;
    case RENDER_OP_TRI:
        bounds = triangle_bounds(words[1], words[2], words[3], decoder->clip);
        break;
    case RENDER_OP_TEX_TRI:
        bounds = triangle_bounds(words[2], words[5], words[8], decoder->clip);
        break;
    case RENDER_OP_SURFACE_QUAD:
        bounds = rect_intersect(logical_rect(words[6], words[7]), decoder->clip);
        break;
    default:
        return RENDER_DECODE_ERROR;
    }
    decoder->index += length;
    out_op->code = code;
    out_op->words = words;
    out_op->length = length;
    out_op->bounds = bounds;
    return RENDER_DECODE_OP;
}

static void plan_remove(render_damage_plan_t *plan, size_t index)
{
    plan->count -= 1U;
    plan->rects[index] = plan->rects[plan->count];
    plan->rects[plan->count] = (render_damage_rect_t){0U, 0U, 0U, 0U};
}

static render_rect_t plan_rect_at(const render_damage_plan_t *plan, size_t index)
{
    const render_damage_rect_t *entry = &plan->rects[index];
    const render_rect_t rect = {(int32_t)entry->x, (int32_t)entry->y,
                                (int32_t)(entry->x + entry->width),
                                (int32_t)(entry->y + entry->height)};
    return rect;
}

/** 把一块区域并进计划：相接的合并，满员时并进放大量最小的那条（同框架策略）。 */
static void plan_add(render_damage_plan_t *plan, render_rect_t bounds, render_rect_t screen)
{
    render_rect_t merged = rect_intersect(bounds, screen);
    if (rect_is_empty(merged)) {
        return;
    }
    size_t index = 0U;
    while (index < plan->count) {
        const render_rect_t existing = plan_rect_at(plan, index);
        if (rect_touches(merged, existing)) {
            merged = rect_union(merged, existing);
            plan_remove(plan, index);
            index = 0U;
        } else {
            index += 1U;
        }
    }
    if (plan->count < RENDER_DAMAGE_MAX_REGIONS) {
        plan->rects[plan->count] = (render_damage_rect_t){
            (uint32_t)merged.x0, (uint32_t)merged.y0, (uint32_t)(merged.x1 - merged.x0),
            (uint32_t)(merged.y1 - merged.y0)};
        plan->count += 1U;
        return;
    }
    size_t best = 0U;
    uint64_t best_inflation = UINT64_MAX;
    for (size_t candidate = 0U; candidate < plan->count; ++candidate) {
        const render_rect_t existing = plan_rect_at(plan, candidate);
        const uint64_t unioned = rect_area(rect_union(existing, merged));
        const uint64_t inflation = unioned - rect_area(existing) - rect_area(merged);
        if (inflation < best_inflation) {
            best_inflation = inflation;
            best = candidate;
        }
    }
    merged = rect_union(merged, plan_rect_at(plan, best));
    plan_remove(plan, best);
    plan_add(plan, merged, screen);
}

/** 两侧解码器状态与下一个 op 是否完全一致：码、字、当前 scissor 与栈都要相同，
 *  否则同一段 op 在两侧落到的像素并不相同，不能跳过。 */
static bool stream_match_at(const render_decoder_t *left, const render_decoder_t *right)
{
    if (left->depth != right->depth || !rect_same(left->clip, right->clip)) {
        return false;
    }
    for (size_t index = 0U; index < left->depth; ++index) {
        if (!rect_same(left->stack[index], right->stack[index])) {
            return false;
        }
    }
    const bool left_end = left->index == left->count;
    const bool right_end = right->index == right->count;
    if (left_end || right_end) {
        return left_end && right_end;
    }
    const size_t left_length = op_word_length(left->words, left->count, left->index);
    const size_t right_length = op_word_length(right->words, right->count, right->index);
    if (left_length == 0U || left_length != right_length) {
        return false;
    }
    if (left->words[left->index] != right->words[right->index]) {
        return false;
    }
    return memcmp(left->words + left->index, right->words + right->index,
                  left_length * sizeof(uint32_t)) == 0;
}

typedef struct {
    uint32_t code;
    size_t offset;
    size_t length;
} render_op_ref_t;

/** 只按 op 字长预扫边界：不推进 scissor，认不出的 op 就地停止。
 *  返回扫出的 op 个数，最多 capacity 个。 */
static size_t scan_ops(const render_decoder_t *decoder, render_op_ref_t *refs, size_t capacity)
{
    size_t index = decoder->index;
    size_t count = 0U;
    while (index < decoder->count && count < capacity) {
        const size_t length = op_word_length(decoder->words, decoder->count, index);
        if (length == 0U || length > decoder->count - index) {
            break;
        }
        refs[count] = (render_op_ref_t){decoder->words[index], index, length};
        count += 1U;
        index += length;
    }
    return count;
}

/** 落点前导：连续 RENDER_RESYNC_RUN 个 op 的字面完全相同，或两侧恰好同时到末尾。
 *  只比 op 码会让大量同码不同字的候选对进入逐字校验，切页时能把一帧拖到几百毫秒；
 *  先按字面筛一遍，进入校验的就只剩真落点。 */
static bool ops_resync_at(const render_decoder_t *left, const render_op_ref_t *left_refs,
                          size_t left_count, size_t left_skip, const render_decoder_t *right,
                          const render_op_ref_t *right_refs, size_t right_count,
                          size_t right_skip)
{
    if (left_skip == left_count && right_skip == right_count) {
        return true;
    }
    if (left_skip == left_count || right_skip == right_count) {
        return false;
    }
    for (size_t step = 0U; step < RENDER_RESYNC_RUN; ++step) {
        if (left_skip + step >= left_count || right_skip + step >= right_count) {
            return true;
        }
        const render_op_ref_t left_ref = left_refs[left_skip + step];
        const render_op_ref_t right_ref = right_refs[right_skip + step];
        if (left_ref.code != right_ref.code || left_ref.length != right_ref.length ||
            memcmp(left->words + left_ref.offset, right->words + right_ref.offset,
                   left_ref.length * sizeof(uint32_t)) != 0) {
            return false;
        }
    }
    return true;
}

/** 跳过 n 个 op，不记 damage；解码失败返回 false。 */
static bool advance_ops(render_decoder_t *decoder, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        render_op_t op;
        if (decoder_next(decoder, NULL, NULL, &op) != RENDER_DECODE_OP) {
            return false;
        }
    }
    return true;
}

/** 在重同步窗口内按「总跨步数最少」找落点：先用 op 码前导筛掉绝大多数候选对，
 *  再对剩下的候选逐字与 scissor 状态校验。返回 false 表示窗口内没有可用落点，
 *  调用方回退整屏。 */
static bool find_resync(const render_decoder_t *left_start, const render_decoder_t *right_start,
                        size_t *out_left_skip, size_t *out_right_skip)
{
    render_op_ref_t left_refs[RENDER_RESYNC_WINDOW + RENDER_RESYNC_RUN];
    render_op_ref_t right_refs[RENDER_RESYNC_WINDOW + RENDER_RESYNC_RUN];
    const size_t left_count =
        scan_ops(left_start, left_refs, RENDER_RESYNC_WINDOW + RENDER_RESYNC_RUN);
    const size_t right_count =
        scan_ops(right_start, right_refs, RENDER_RESYNC_WINDOW + RENDER_RESYNC_RUN);
    /* 逐字校验的上限：落点被前导筛过一遍后通常只剩一两个候选，这里是兜底，
     * 避免构造得极端的两帧把一次差分拖到毫秒级。 */
    uint32_t verify_budget = 8U;
    for (size_t total = 1U; total <= RENDER_RESYNC_WINDOW * 2U; ++total) {
        for (size_t left_skip = 0U; left_skip <= total && left_skip <= RENDER_RESYNC_WINDOW;
             ++left_skip) {
            const size_t right_skip = total - left_skip;
            if (right_skip > RENDER_RESYNC_WINDOW || left_skip > left_count ||
                right_skip > right_count) {
                continue;
            }
            if (!ops_resync_at(left_start, left_refs, left_count, left_skip, right_start,
                               right_refs, right_count, right_skip)) {
                continue;
            }
            if (verify_budget == 0U) {
                return false;
            }
            verify_budget -= 1U;
            render_decoder_t left = *left_start;
            render_decoder_t right = *right_start;
            if (!advance_ops(&left, left_skip) || !advance_ops(&right, right_skip)) {
                continue;
            }
            if (!stream_match_at(&left, &right)) {
                continue;
            }
            *out_left_skip = left_skip;
            *out_right_skip = right_skip;
            return true;
        }
    }
    return false;
}

/** 跳过 n 个 op，并把它们的范围记进 damage。 */
static bool skip_ops(render_decoder_t *decoder, size_t count, render_damage_plan_t *plan,
                     render_damage_font_fn font_lookup, void *font_user_data)
{
    for (size_t index = 0U; index < count; ++index) {
        render_op_t op;
        const render_decode_result_t result =
            decoder_next(decoder, font_lookup, font_user_data, &op);
        if (result != RENDER_DECODE_OP) {
            return false;
        }
        plan_add(plan, op.bounds, decoder->screen);
    }
    return true;
}

bool render_damage_diff(const uint32_t *previous, size_t previous_words,
                        const uint32_t *current, size_t current_words,
                        uint32_t viewport_width, uint32_t viewport_height,
                        render_damage_font_fn font_lookup, void *font_user_data,
                        render_damage_plan_t *out_plan)
{
    if (out_plan == NULL || previous == NULL || current == NULL || viewport_width == 0U ||
        viewport_height == 0U || viewport_width > (uint32_t)INT32_MAX ||
        viewport_height > (uint32_t)INT32_MAX) {
        return false;
    }
    if (previous_words == 0U || current_words == 0U) {
        return false;
    }
    out_plan->count = 0U;
    if (previous_words == current_words &&
        memcmp(previous, current, previous_words * sizeof(uint32_t)) == 0) {
        return true;
    }

    const render_rect_t screen = {0, 0, (int32_t)viewport_width, (int32_t)viewport_height};
    render_decoder_t left = {previous, previous_words, 0U, screen, screen, {{0, 0, 0, 0}}, 0U};
    render_decoder_t right = {current, current_words, 0U, screen, screen, {{0, 0, 0, 0}}, 0U};
    for (;;) {
        if (stream_match_at(&left, &right)) {
            if (left.index == left.count) {
                return left.depth == 0U && right.depth == 0U;
            }
            render_op_t op;
            if (decoder_next(&left, NULL, NULL, &op) != RENDER_DECODE_OP ||
                decoder_next(&right, NULL, NULL, &op) != RENDER_DECODE_OP) {
                return false;
            }
            continue;
        }
        size_t left_skip = 0U;
        size_t right_skip = 0U;
        if (!find_resync(&left, &right, &left_skip, &right_skip)) {
            return false;
        }
        if (left_skip == 0U && right_skip == 0U) {
            return false;
        }
        if (!skip_ops(&left, left_skip, out_plan, font_lookup, font_user_data) ||
            !skip_ops(&right, right_skip, out_plan, font_lookup, font_user_data)) {
            out_plan->count = 0U;
            return false;
        }
    }
}
