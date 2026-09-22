/**
 * 本机 damage 差分：把两帧 draw list 的真实差异折成待重画矩形。
 *
 * 框架的 damage 计划遇到结构变化（切页、焦点环出现或移动、弹窗开关）会直接退回
 * 整屏重画，而应用侧真正变化的常常只是其中一小块。本模块按 DrawList v1 的逐 op
 * 字长解出每个 op 的屏幕范围，用「逐 op 对齐 + 窗口内重同步」找出两帧之间被改动、
 * 新增与移除的 op，只把这些 op 的范围交回调用方。
 *
 * 边界：只算区域、不渲染；op 解码失败、scissor 不配对、窗口内找不到重同步落点时
 * 返回 false，调用方必须回退整屏计划。op 字长与坐标打包见 PocketJS 的
 * contracts/spec/spec.ts（DrawList v1）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 一次差分给出的区域数上限：与 PocketJS 的 MAX_DAMAGE_REGIONS 一致。 */
#define RENDER_DAMAGE_MAX_REGIONS 8

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} render_damage_rect_t;

/** 待重画区域表；count 为 0 表示两帧逐字一致，面板内容无需改动。 */
typedef struct {
    uint32_t count;
    render_damage_rect_t rects[RENDER_DAMAGE_MAX_REGIONS];
} render_damage_plan_t;

/** 字形图集查询：给出 slot 的字格宽高与字形数，用于字形运行 op 的范围。
 *  返回 false 表示 slot 未知，该 op 退化成整块 scissor（保守，宁可多画）。 */
typedef bool (*render_damage_font_fn)(void *user_data, uint32_t slot,
                                      uint32_t *cell_width, uint32_t *cell_height,
                                      uint32_t *glyph_count);

/** 比对 previous 与 current 两帧 draw list 的实际差异。
 *  previous 必须是「面板当前内容」对应的那一帧，否则会漏画。
 *  返回 false 表示无法给出可靠结果，调用方应回退整屏重画。 */
bool render_damage_diff(const uint32_t *previous, size_t previous_words,
                        const uint32_t *current, size_t current_words,
                        uint32_t viewport_width, uint32_t viewport_height,
                        render_damage_font_fn font_lookup, void *font_user_data,
                        render_damage_plan_t *out_plan);

/** 单个 op 的字数（含头部字），从 words[at] 起算；认不出或长度越界返回 0。
 *  供逐帧追踪这类诊断按 op 走一遍 draw list，渲染路径不需要它。 */
size_t render_damage_op_length(const uint32_t *words, size_t count, size_t at);

#ifdef __cplusplus
}
#endif
