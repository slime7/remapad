#include "layout.h"

/** DualShock 3：按键全在按键位图里，方向键也是——没有帽子开关。Select 填触摸板位
 *  （目标侧作减号）、Start 填选项位，与 PS 家族其余型号的位置语义一致；L2 / R2 的
 *  数字位不映射，扳机走同字的压力值。 */
static const uint32_t s_btn_map[24] = {
    PAD_BTN_TOUCHPAD, PAD_BTN_L3, PAD_BTN_R3, PAD_BTN_OPT,
    PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_LEFT,
    0, 0, PAD_BTN_L1, PAD_BTN_R1,
    PAD_BTN_TRIANGLE, PAD_BTN_CIRCLE, PAD_BTN_CROSS, PAD_BTN_SQUARE,
    PAD_BTN_HOME, 0, 0, 0,
    0, 0, 0, 0,
};

/** DualShock 3（0x054C:0x0268）：有线与蓝牙都报 0x01、字段偏移一致（蓝牙多一层
 *  传输头，剥掉后与有线相同），因此一行覆盖两种连接。运动字段是 41-46 的大端加
 *  速度加 47-48 的陀螺，与解析器要求的 6×int16 小端不同；电量也不在输入报告里
 *  （要靠特性报告查询），两处都不登记。按键位是否为低电平有效（0 表示按下）与
 *  蓝牙是否多一字节前缀，都要等 `pc/remapadctl.py --dump` 实测确认，当前按高电平
 *  有效、49 字节形式登记。 */
static const pad_layout_t s_rows[] = {
    {
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x01,
        .pids = {0x0268},
        .buttons_off = 5,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {12, 13},
        .stick_off = {1, 2, 3, 4},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_btn_map,
        /* 输出报告 0x01（48 字节）：b2/b3 是右小马达的时长与强度、b4/b5 是左大
         * 马达的时长与强度，时长写 0xFF 表示保持到下一条命令。偏移取自公开实现，
         * 未实机核对；LED 控制要另走 SET_REPORT 序列，本轮不映射。 */
        .out = {
            .report_id = 0x01,
            .len = 48,
            .presets = {{2, 0xFF}, {4, 0xFF}},
            .rumble_off = {5, 3},
            .rumble_max = {255, 255},
            .led_style = PAD_LED_NONE,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
        },
    },
};

const pad_layout_module_t pad_layout_module_ds3 = {
    .name = "ds3",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
