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
         * 偏移由蓝牙那行的实测值减去两字节前缀换算，抓包核对前作初值；电量
         * 字节尚未核对，本轮不登记。触摸点按 Linux hid-playstation.c 的公共段
         * 布局登记（传感器时间戳与保留字节之后，每点 4 字节，第一个触点在偏移
         * 33），实机抓包尚未核对。 */
        .family = PAD_FAMILY_PS,
        .conn = PAD_CONN_USB,
        .report_id = 0x01,
        .pids = {0x0CE6, 0x0DF2},
        .buttons_off = 8,
        .buttons_bytes = 3,
        .hat_off = 8,
        .trigger_off = {5, 6},
        .stick_off = {1, 2, 3, 4},
        .touch_off = 33,
        .motion_off = 16,
        .battery_off = PAD_OFF_NONE,
        .touch_max_x = 1919,
        .touch_max_y = 1079,
        /* 耳机状态字节按蓝牙行实测值减一字节前缀换算（本文件各字段的换算
         * 关系），有线形态尚未抓包核对，核对前不登记。 */
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 输出报告 0x02（48 字节 = Report ID + 47 字节公共段，与 SDL 在
         * Windows 上发的长度一致）：b1/b2 是两个 valid_flag（b1：0x01 兼容
         * 震动、0x02 关音频触觉、0x20 更新喇叭音量、0x80 更新音频控制；b2：
         * 0x10 玩家指示灯），b3/b4 是右小马达与左大马达，b6 是喇叭音量，
         * b44 是玩家灯、b45-b47 是灯条 RGB。
         * 偏移取 Linux hid-playstation.c 的 dualsense_output_report_usb，蓝牙行
         * 实机核对过震动与灯。灯条不驱动（实机：每次震动写回都把
         * 灯条钉成玩家蓝、平时淡回默认白，一震就变色）——valid_flag1 只置玩家
         * 灯位，灯条设置控制与 RGB 字节全零、不声明有效，颜色留给 PC 侧管理；
         * 玩家号落四颗白灯（led_mask_map）。喇叭音量逐报钉在 100（PS5 缺省
         * 档，vds/DS5Dongle 的初始状态同值）：手柄自己的音量档被主机/PC 游戏
         * 压低时采样提示音会轻到听不见（实机：查找手柄短鸣非常轻）。 */
        .out = {
            .report_id = 0x02,
            .len = 48,
            .presets = {{1, 0x23}, {2, 0x10}, {6, 100}},
            .rumble_off = {4, 3},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 44,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            .audio_haptic = 1,
            /* HD 触觉波形映射（NS 波形规则 → 本设备 PCM）：USB 承载是 4 声道
             * 48kHz 16-bit PCM（频道 3/4 直连左右触觉音圈，频道 1/2 是手柄
             * 小喇叭）。主机的时序子帧按时间顺序重整进音圈（15ms 周期各播
             * 1/3），频率按 9 位 log2 刻度解出 Hz 后夹进音圈的有效频段
             * （20-500Hz，码 0 回落 80/135——BlueRetro 驱动常量 0x180/0x1E1
             * 的落地值）；采样音色的强震段以 135Hz（音圈静置频率，≈共振点）
             * 铺音圈、发声段以 500Hz 铺扬声器并折进音圈（蓝牙通路没有
             * 扬声器通道，两条承载的音圈行为保持一致）。amp_peak 取 30000
             * （满量程 32767 的 91%）：音圈与喇叭的响度上限，游戏内振幅
             * 低（实抓中位 6/255）不受裁剪影响。 */
            .hd = {.ops = 3, .rate_hz = 48000, .amp_peak = 30000, .cycle_ms = 15,
                   .lf_min_hz = 20, .lf_max_hz = 500, .lf_default_hz = 80,
                   .hf_min_hz = 20, .hf_max_hz = 500, .hf_default_hz = 135,
                   .pulse_hz = 135, .beep_hz = 500},
            .led_mask_map = {0x04, 0x0A, 0x15, 0x1B},
        },
    },
    {
        /* 蓝牙（0x31）：比 DS4 蓝牙的 0x11 整体后移一位。偏移为 DualSense Edge
         * 实测抓包：静止帧第 9 字节读作 0x08（帽子开关松开）、四轴落在死区内、
         * 第 17-22 字节的角速度接近 0 而加速度有一轴约 1 g。触摸点按公共段
         * 布局登记（第一个触点在偏移 34），实机抓包尚未核对。第 55 字节是
         * 耳机状态：插拔差分实测 0x00（未插入）/
         * 0x01（插入）/ 0x03（插入带麦），第 56 字节跟着 bit0 走。电量在第
         * 54 字节：两份抓包分别读作 0x09（90%）与
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
        .touch_off = 34,
        .motion_off = 17,
        .battery_off = 54,
        .touch_max_x = 1919,
        .touch_max_y = 1079,
        .headset_off = 55,
        .headset_style = PAD_HEADSET_PS,
        .stick_style = PAD_STICK_U8,
        .caps = PAD_CAP_MOTION | PAD_CAP_TOUCHPAD | PAD_CAP_TRIGGER_ANALOG |
                PAD_CAP_RUMBLE | PAD_CAP_BATTERY | PAD_CAP_MIC,
        .invert_y = true,
        .btn_map = pad_ps_btn_map,
        .motion = {.samples = 1, .stride = 12},
        /* 蓝牙形态报告 0x31（78 字节）：b1 是序号/标签字节（高半字节逐报
         *  递增、低半字节 tag 保持 0，内核 hid-playstation.c 注明「每份报告
         *  都要递增」，恒值会被手柄按重复包处理——seq_off 交给编码器递增）、
         *  b2 是固定魔数 0x10、公共段从 b3 起，末 4 字节是 CRC32——缺这段主机
         *  整份报告都不认（实机表现：写回成功而手柄毫无反应）。b46 是玩家灯、
         *  b47-b49 是灯条 RGB，偏移取 Linux dualsense_output_report_bt。灯条不
         *  驱动（同有线行，实机把灯条钉成玩家蓝 + 淡出设置，一震就
         *  变色）：valid_flag1 只置玩家灯位，灯条字节全零。 */
        .out = {
            .report_id = 0x31,
            .len = 78,
            /* b3 带 0x20（更新喇叭音量）、b8 钉 100：与有线行同一理由——
             * 蓝牙接入时 0x31 写回也要保住手柄喇叭的音量档（0x36 私有流的
             * 状态块另带一份）。 */
            .presets = {{2, 0x10}, {3, 0x23}, {4, 0x10}, {8, 100}},
            .rumble_off = {6, 5},
            .rumble_max = {255, 255},
            .rumble_band = {PAD_RUMBLE_LF, PAD_RUMBLE_HF},
            .led_mask_off = 46,
            .led_rgb_off = PAD_OFF_NONE,
            .led_style = PAD_LED_PLAYER_MASK,
            /* 蓝牙私有触觉流（SAxense 逆向的 0x32 报告，141 字节 = 报文头 +
             *  packet 0x11 配置/序号 + packet 0x12 承载 64 字节 PCM + CRC32）：
             *  3000Hz / 2 声道 / 8-bit，每 10.67ms 一报，PC 侧按 FEEDBACK 的
             *  HD 子帧哑渲染。audio_haptic 置位后 0x31 的马达字节让位，同一
             *  对音圈不双驱动。 */
            .audio_haptic = 1,
            .hd = {.ops = 3, .rate_hz = 3000, .amp_peak = 127, .cycle_ms = 15,
                   .lf_min_hz = 20, .lf_max_hz = 500, .lf_default_hz = 80,
                   .hf_min_hz = 20, .hf_max_hz = 500, .hf_default_hz = 135,
                   .pulse_hz = 135, .beep_hz = 500},
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
