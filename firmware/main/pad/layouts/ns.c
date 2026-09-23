#include "layout.h"

/**
 * Nintendo 家族：Switch 一代手柄（0x30 标准报文 / 0x3F 简单报文）与 Switch 2 手柄
 * （0x05 / 0x09 报文体），以及伪装成 NS 布局的第三方手柄；行里的 native_lang 决定
 * 能否原样转发给同代目标。USB 形态首字节是 Report ID，各行偏移比报文体自身大 1；
 * 两代 NS1 报文的按键与摇杆偏移互不相同，核对状态见 docs/controller-ns1.md；
 * 6 轴样本按私有格式的统一刻度解析（NS 家族的标称值），这些行都不声明换算比。
 */

/** NS1（0x30）标准报文按键位：右半边、功能键、左半边各一字节，两侧按键可叠在同一行。 */
static const uint32_t s_ns1_btn_map[24] = {
    /* b0：Y / X / B / A / SR / SL / R / ZR。面键按位置取值。 */
    PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, 0, 0, PAD_BTN_R1, 0,
    /* b1：Minus / Plus / RStick / LStick / Home / Capture / 保留 / 充电握把。 */
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_R3, PAD_BTN_L3, PAD_BTN_HOME, PAD_BTN_SHARE, 0, 0,
    /* b2：Down / Up / Right / Left / SR / SL / L / ZL。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, 0, 0,
    PAD_BTN_L1, 0,
};

/**
 * NS1（0x3F）简单报文按键位：两字节，首字节是这一侧的按键（左手柄是四向键、
 * 右手柄是面键），次字节两侧共用——L/R 与 ZL/ZR 按手柄侧取含义，
 * 因此左右各一份表。ZL / ZR 是数字位，由行的 trigger_btn 声明成满量程扳机。
 */
static const uint32_t s_ns1_simple_l_btn_map[16] = {
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, PAD_BTN_DPAD_UP, 0, 0, 0, 0,
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_L3, PAD_BTN_R3, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_L1, 0,
};

static const uint32_t s_ns1_simple_r_btn_map[16] = {
    PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, 0, 0, 0, 0,
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_L3, PAD_BTN_R3, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_R1, 0,
};

/**
 * NS2（0x09 报文体）按键位：b0 右半边、b1 左半边、b2 系统与背键。
 * ZL / ZR 是数字位，由行的 trigger_btn 声明成满量程扳机（透传路径不受影响）。
 */
static const uint32_t s_ns2_09_btn_map[24] = {
    /* b0：B / A / Y / X / R / ZR / Plus / RStick。 */
    PAD_BTN_CROSS, PAD_BTN_CIRCLE, PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_R1, 0,
    PAD_BTN_OPT, PAD_BTN_R3,
    /* b1：Down / Right / Left / Up / L / ZL / Minus / LStick。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, PAD_BTN_DPAD_UP, PAD_BTN_L1, 0,
    PAD_BTN_TOUCHPAD, PAD_BTN_L3,
    /* b2：Home / Capture / GR / GL / C / 保留。 */
    PAD_BTN_HOME, PAD_BTN_SHARE, PAD_BTN_R4, PAD_BTN_L4, PAD_BTN_MUTE, 0, 0, 0,
};

/** NS2（0x05 报文体）按键位：四字节，第三字节还带左半边的 SL/SR 与 C 键。 */
static const uint32_t s_ns2_05_btn_map[32] = {
    /* b0：Y / X / B / A / 右SL / 右SR / R / ZR。 */
    PAD_BTN_SQUARE, PAD_BTN_TRIANGLE, PAD_BTN_CROSS, PAD_BTN_CIRCLE, 0, 0, PAD_BTN_R1, 0,
    /* b1：Minus / Plus / RStick / LStick / Home / Capture / C / 保留。 */
    PAD_BTN_TOUCHPAD, PAD_BTN_OPT, PAD_BTN_R3, PAD_BTN_L3, PAD_BTN_HOME, PAD_BTN_SHARE,
    PAD_BTN_MUTE, 0,
    /* b2：Down / Up / Right / Left / 左SR / 左SL / L / ZL。 */
    PAD_BTN_DPAD_DOWN, PAD_BTN_DPAD_UP, PAD_BTN_DPAD_RIGHT, PAD_BTN_DPAD_LEFT, 0, 0,
    PAD_BTN_L1, 0,
    /* b3：GR / GL / 其余保留。 */
    PAD_BTN_R4, PAD_BTN_L4, 0, 0, 0, 0, 0, 0,
};

/**
 * NS1 的震动走输出报告 0x10：每侧 4 字节，高频与低频各有自己的频率与振幅编码，
 * 因此直接吃主机下发的 LRA 波形（频率码与振幅码的落地规则见 docs/controller-ns1.md）。
 * 玩家灯与控制灯本轮不映射。
 */
static const pad_layout_t s_rows[] = {
    {
        /* Switch 一代 Pro Controller（0x30）：有线与蓝牙的报文体一致，字段偏移相同，
         * 因此一行覆盖两种连接。摇杆是 12 位紧凑打包三字节、运动样本加速在前、
         * 电量在报文体第 2 字节，ZL / ZR 是数字位（按满量程扳机填）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x30,
        .pids = {0x2009},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {5, 3},
        .trigger_btn_bit = {7, 7},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 13,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS1,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE | PAD_CAP_BATTERY,
        .invert_y = true,
        .btn_map = s_ns1_btn_map,
        .motion = {.samples = 3, .stride = 12, .accel_first = true},
        .out = {
            .report_id = 0x10,
            .len = 9,
            .rumble_off = {1, 5},
            .rumble_style = PAD_RUMBLE_NS1_WAVE,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 一代 Joy-Con (L)（0x3F 简单报文）：按键两字节在报文体 1-2，
         * 摇杆是两对 16 位小端（中心 0x8000）占左手柄那一槽；报文里没有电量与
         * 运动区段，均不解析。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x3F,
        .pids = {0x2006},
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {2, PAD_OFF_NONE},
        .trigger_btn_bit = {7, 0},
        .stick_off = {4, 6, PAD_OFF_NONE, PAD_OFF_NONE},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns1_simple_l_btn_map,
        .out = {
            .report_id = 0x10,
            .len = 9,
            .rumble_off = {1, 5},
            .rumble_style = PAD_RUMBLE_NS1_WAVE,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 一代 Joy-Con (R)（0x3F 简单报文，右手柄独立 PID）：摇杆占右手柄
         * 那一槽（两对 16 位小端），首字节是面键；电量与运动区段同样不解析。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_UNKNOWN,
        .report_id = 0x3F,
        .pids = {0x2007},
        .buttons_off = 1,
        .buttons_bytes = 2,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {PAD_OFF_NONE, 2},
        .trigger_btn_bit = {0, 7},
        .stick_off = {PAD_OFF_NONE, PAD_OFF_NONE, 8, 10},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U16,
        .caps = PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns1_simple_r_btn_map,
        .out = {
            .report_id = 0x10,
            .len = 9,
            .rumble_off = {1, 5},
            .rumble_style = PAD_RUMBLE_NS1_WAVE,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS1,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
    {
        /* Switch 2 Pro Controller（0x09 报文体）：按键 3 字节、摇杆 12 位
         * 打包、电量在 0x01。运动块在 0x0F（40 字节），公开资料里仍是未解析
         * 的打包格式，因此不登记——手柄的运动数据靠透传原样送达主机。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2069},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {4, 3},
        .trigger_btn_bit = {5, 5},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
            /* 同代透传：主机下发的 32 字节 LRA 参数包按 USB 形态原样写回。 */
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 2 Joy-Con (L)（0x09 报文体）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2067},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {4, 3},
        .trigger_btn_bit = {5, 5},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 2 Joy-Con (R)（0x09 报文体）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x09,
        .pids = {0x2066},
        .buttons_off = 3,
        .buttons_bytes = 3,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {4, 3},
        .trigger_btn_bit = {5, 5},
        .stick_off = {6, 6, 9, 9},
        .touch_off = PAD_OFF_NONE,
        .motion_off = PAD_OFF_NONE,
        .battery_off = 2,
        .stick_style = PAD_STICK_U12,
        .battery_style = PAD_BATTERY_NS2,
        .caps = PAD_CAP_BATTERY | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_09_btn_map,
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
    {
        /* Switch 2 Pro Controller 的 0x05 报文体（主机切到通用报告时）。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2069},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {7, 5},
        .trigger_btn_bit = {7, 7},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_PRO,
    },
    {
        /* Switch 2 Joy-Con (L) 的 0x05 报文体。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2067},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {7, 5},
        .trigger_btn_bit = {7, 7},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_L,
    },
    {
        /* Switch 2 Joy-Con (R) 的 0x05 报文体。 */
        .family = PAD_FAMILY_NS,
        .conn = PAD_CONN_USB,
        .report_id = 0x05,
        .pids = {0x2066},
        .buttons_off = 5,
        .buttons_bytes = 4,
        .hat_off = PAD_OFF_NONE,
        .trigger_off = {PAD_OFF_NONE, PAD_OFF_NONE},
        .trigger_btn_off = {7, 5},
        .trigger_btn_bit = {7, 7},
        .stick_off = {11, 11, 14, 14},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 43,
        .battery_off = PAD_OFF_NONE,
        .stick_style = PAD_STICK_U12,
        .caps = PAD_CAP_MOTION | PAD_CAP_RUMBLE,
        .invert_y = true,
        .btn_map = s_ns2_05_btn_map,
        .motion = {.samples = 1, .stride = 12},
        .out = {
            .report_id = 0,
            .led_style = PAD_LED_NONE,
        },
        .native_lang = PAD_LANG_NS2,
        .native_identity = PAD_IDENTITY_JOYCON_R,
    },
};

const pad_layout_module_t pad_layout_module_ns = {
    .name = "ns",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
