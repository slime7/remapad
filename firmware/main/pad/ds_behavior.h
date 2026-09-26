#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DS4 / DS5 手柄行为（屏幕「DS4、DS5 设置」页两项开关的落地）：
 * 触摸板映射加减键（默认关）按先触发的半区发减号或加号，截图键（默认开）让触摸板按下发截图。
 * 两项只作用于 PS 家族的触摸板按下位；位置取不到时退回截图键那一档，
 * 键位在按下那一刻定一次、按住期间不变。键位判定流程见 docs/controller-ps.md。
 */

/** 设置页两项开关的取值（固件侧由 app_config 持久化）。 */
typedef struct {
  bool touchpad_plus_minus;
  bool capture_key;
} pad_ds_config_t;

/** 跨采样状态：触点触发先后的比较与按下期间的键位锁存。 */
typedef struct {
  /** 采样序号：每次 apply 自增，只用来比较触发先后。 */
  uint32_t tick;
  /** 各半区当前这一路触点的触发时刻（0 = 这一路没有触点）。 */
  uint32_t onset[PAD_TOUCH_COUNT];
  /** 各半区上一拍是否贴着手指（下沿检测用）。 */
  bool held[PAD_TOUCH_COUNT];
  /** 本次触摸板按下定下的键位（0 = 还没定）；见文件头。 */
  uint32_t mapped;
} pad_ds_state_t;

/** 复位跨采样状态：数据面任务启动与主机端用例的入口。 */
void pad_ds_reset(pad_ds_state_t *state);

/** 按配置改写一帧私有状态里的触摸板按键位（原地，只动按键位）。 */
void pad_ds_apply(pad_ds_state_t *state, const pad_ds_config_t *config, pad_state_t *pad);

#ifdef __cplusplus
}
#endif
