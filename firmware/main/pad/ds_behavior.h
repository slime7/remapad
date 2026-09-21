#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DS4 / DS5 手柄行为（屏幕「DS4、DS5 设置」页两项开关的落地）：
 *
 * - 触摸板映射加减键（默认关）：触摸板按下时按「先触发」那一半的位置发键，
 *   左半发减号、右半发加号，这一路原本的截图不再发；
 * - 截图键（默认开）：触摸板按下发截图，关掉后这一路改发减号。
 *
 * 两项都只作用于 PS 家族的触摸板按下（DS4 与 DualSense 的触摸板按下落在
 * PAD_BTN_SHARE）：DS3、Xbox Series 的分享键与 NS 手柄的触摸板位不受影响。
 * 位置取不到时（该帧没有触摸数据）退回截图键开关那一档。
 *
 * 键位在按下那一刻定一次，按住期间不变：中途另一根手指落下或抬手不会让已经
 * 发出去的键来回跳；设置改动同样在下次按下时生效。
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
