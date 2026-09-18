#include "layout.h"

/**
 * DualSense 与 DualSense Edge（0x0CE6 / 0x0DF2）：有线报 0x01、蓝牙报 0x31，两者
 * 只差两字节前缀，位序与 DS4 相同（共用 pad_ps_btn_map），Edge 的背键在第三字节
 * 的高两位、左右 Fn 键不映射（它们兼作配置档修饰键）。
 */
static const pad_layout_t s_rows[] = {
    {
        /* 有线（64 字节）：第 8 字节低四位是方向键帽子开关、高四位是面键，第 9 字节
         * 是肩键、Create/Options 与摇杆按下，第 10 字节是 PS、触摸板按下与静音键。
         * 偏移由蓝牙那行的实测值减去两字节前缀换算，抓包核对前作初值；触摸点
         * （每点 4 字节）与电量字节尚未核对，本轮不登记。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 8,
        .buttons_bytes = 3,
        .hat_off = 8,
        .trigger_off = {5, 6},
        .stick_off = {1, 2, 3, 4},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 16,
        .battery_off = PAD_OFF_NONE,
        /* 耳机状态字节按蓝牙行实测值减一字节前缀换算（本文件各字段的换算
         * 关系），有线形态尚未抓包核对，核对前不登记。 */
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 输出报告 0x02（48 字节 = Report ID + 47 字节公共段，与 SDL 在
         * Windows 上发的长度一致）：b1/b2 是两个 valid_flag（0x01 兼容震动、
         * 0x02 关音频触觉、0x04 灯条、0x10 玩家指示灯），b3/b4 是右小马达与
         * 左大马达，b44 是玩家灯、b45-b47 是灯条 RGB。
         * 偏移取 Linux hid-playstation.c 的 dualsense_output_report_usb，蓝牙行
         * 实机核对过震动与灯。灯条不驱动（2026-09-18 实机：每次震动写回都把
         * 灯条钉成玩家蓝、平时淡回默认白，一震就变色）——valid_flag1 只置玩家
         * 灯位，灯条设置控制与 RGB 字节全零、不声明有效，颜色留给 PC 侧管理；
         * 玩家号落四颗白灯（led_mask_map）。 */
        .out = {
            .report_id = 0x02,
            .len = 48,
            .presets = {{1, 0x03}, {2, 0x10}},
            .rumble_off = {4, 3},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 44,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
            .audio_haptic = 1,
            .led_mask_map = {0x04, 0x0A, 0x15, 0x1B},
        },
    },
    {
        /* 蓝牙（0x31）：比 DS4 蓝牙的 0x11 整体后移一位。偏移为 DualSense Edge
         * 实测抓包：静止帧第 9 字节读作 0x08（帽子开关松开）、四轴落在死区内、
         * 第 17-22 字节的角速度接近 0 而加速度有一轴约 1 g。触摸点尚未核对，
         * 本轮不登记。第 55 字节是耳机状态：插拔差分实测 0x00（未插入）/
         * 0x01（插入）/ 0x03（插入带麦），第 56 字节跟着 bit0 走。电量在第
         * 54 字节：2026-09-15 与 2026-09-17 两份抓包分别读作 0x09（90%）与
         * 0x05（50%），同一期间耳机字节都在原位，与 DS4 的电量字节同一套
         * 读法（低四位 0-10 档、bit4 充电中）。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_BT,
        .report_id = 0x31,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 9,
        .buttons_bytes = 3,
        .hat_off = 9,
        .trigger_off = {6, 7},
        .stick_off = {2, 3, 4, 5},
        .touch_off = PAD_OFF_NONE,
        .motion_off = 17,
        .battery_off = 54,
        .headset_off = 55,
        .headset_style = PAD_HEADSET_PS,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TRIGGER_ANALOG | PAD_CAP_RUMBLE |
                PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 蓝牙形态报告 0x31（78 字节）：b1 是序号/标签字节（高半字节逐报
         *  递增、低半字节 tag 保持 0，内核 hid-playstation.c 注明「每份报告
         *  都要递增」，恒值会被手柄按重复包处理——seq_off 交给编码器递增）、
         * b2 是固定魔数 0x10、公共段从 b3 起，末 4 字节是 CRC32——缺这段主机
         * 整份报告都不认（实机表现：写回成功而手柄毫无反应）。b46 是玩家灯、
         * b47-b49 是灯条 RGB，偏移取 Linux dualsense_output_report_bt。灯条不
         * 驱动（同有线行，2026-09-18 实机把灯条钉成玩家蓝 + 淡出设置，一震就
         * 变色）：valid_flag1 只置玩家灯位，灯条字节全零。 */
        .out = {
            .report_id = 0x31,
            .len = 78,
            .presets = {{2, 0x10}, {3, 0x03}, {4, 0x10}},
            .rumble_off = {6, 5},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 46,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            .haptic = PAD_HAPTIC_AS_RUMBLE,
            .frame = PAD_OUT_FRAME_PS_BT,
            .seq_off = 1,
            .led_mask_map = {0x04, 0x0A, 0x15, 0x1B},
        },
    },
};

const pad_layout_module_t pad_layout_module_ds5 = {
    .name = "ds5",
    .rows = s_rows,
    .row_count = sizeof(s_rows) / sizeof(s_rows[0]),
};
