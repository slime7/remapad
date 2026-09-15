#include "layout.h"

/** Xbox 系列：有线的字段偏移按公开资料填、蓝牙待抓包核对。 */
static const pad_layout_t s_rows[] = {
    {
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_USB,
        .report_id = 0x00,
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {3, 4},
        .stick_off = {5, 7, 9, 11},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_I16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        /* Xbox 的 Y 轴沿用 XInput 语义（正为上），不需要翻转。 */
        .invert_y = false,
        .btn_map = pad_xbox_btn_map,
    },
    {
        /* Xbox 蓝牙：字段顺序与有线一致，偏移待抓包核对。 */
        .family = PAD_FAMILY_XBOX,
        .conn = PAD_CONN_BT,
        .report_id = 0x01,
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {3, 4},
        .stick_off = {5, 7, 9, 11},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_I16,
        .caps = PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE,
        .invert_y = false,
        .btn_map = pad_xbox_btn_map,
    },
};

const pad_layout_module_t pad_layout_module_xbox = {
    .name = "xbox",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};

