#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 私有手柄格式（输入通路的中间模型，见 docs/ABSTRACTIONS.md「输入通路」）：
 * 各家手柄（Xbox / PS / Steam）的报告先由 pad_device 解析成这份模型，再由
 * target/ 下的目标编码器转成目标手柄报文。模型与任何目标家族解耦，因此
 * 新增手柄只改家族表，新增目标只加一份编码器。
 *
 * 坐标约定：四轴与双扳机统一为 0-4095 整数，摇杆中位 `PAD_AXIS_CENTER`；
 * 轴向按数学意义取正——X 向右为正、Y 向上为正，输入侧负责把各家相反的
 * Y 轴翻过来。面键沿用 PS 的键名按位置固定：Triangle 上、Circle 右、Cross 下、
 * Square 左——Xbox 与 Nintendo 的 A/B/X/Y 标签位置各不相同（Xbox 的 A 在下、
 * Nintendo 的 A 在右），用 PS 名可以避免「A 到底是哪个键」这类混淆，家族表
 * 负责把各家的物理键填到对应位置。
 */

#define PAD_AXIS_MIN 0
#define PAD_AXIS_CENTER 2048
#define PAD_AXIS_MAX 4095

/** 手柄家族：决定报告布局与物理键位置。 */
typedef enum {
    PAD_FAMILY_UNKNOWN = 0,
    PAD_FAMILY_XBOX,
    PAD_FAMILY_PS,
    PAD_FAMILY_STEAM,
    PAD_FAMILY_COUNT,
} pad_family_t;

/** 连接方式：同一型号在 USB 与蓝牙下的报告布局不同（长度与偏移都会变）。 */
typedef enum {
    PAD_CONN_UNKNOWN = 0,
    PAD_CONN_USB,
    PAD_CONN_BT,
} pad_conn_t;

/** 按键位（位置语义，键名沿用 PS）。扩展位放主机侧新增或第三方手柄的附加键。 */
enum {
    PAD_BTN_TRIANGLE = 1u << 0, /**< △ 上 */
    PAD_BTN_CIRCLE = 1u << 1,   /**< ○ 右 */
    PAD_BTN_CROSS = 1u << 2,    /**< ✕ 下 */
    PAD_BTN_SQUARE = 1u << 3,   /**< □ 左 */
    PAD_BTN_LB = 1u << 4,
    PAD_BTN_RB = 1u << 5,
    PAD_BTN_LSTICK = 1u << 6,
    PAD_BTN_RSTICK = 1u << 7,
    /** 触摸板按下：Xbox 的 View（select）与 PS 的触摸板按下都落在这里，
     *  目标侧作减号。 */
    PAD_BTN_TOUCHPAD = 1u << 8,
    /** 选项键：Xbox 的 Menu 与 PS 的 Options 都落在这里，目标侧作加号。 */
    PAD_BTN_OPT = 1u << 9,
    /** 主页键：Xbox 的西瓜键与 PS 的 PS 键都落在这里。 */
    PAD_BTN_HOME = 1u << 10,
    /** 分享键：Xbox 的分享键与 PS 的 Create 都落在这里，目标侧作截图。 */
    PAD_BTN_SHARE = 1u << 11,
    PAD_BTN_DPAD_UP = 1u << 12,
    PAD_BTN_DPAD_DOWN = 1u << 13,
    PAD_BTN_DPAD_LEFT = 1u << 14,
    PAD_BTN_DPAD_RIGHT = 1u << 15,
    PAD_BTN_L4 = 1u << 16,
    PAD_BTN_L5 = 1u << 17,
    PAD_BTN_R4 = 1u << 18,
    PAD_BTN_R5 = 1u << 19,
    /** 静音键：只有 PS 的 DualSense 有，目标侧作 C 键。 */
    PAD_BTN_MUTE = 1u << 20,
};

/** 轴索引（pad_state_t.axis，顺序固定）。 */
enum {
    PAD_AXIS_LX = 0,
    PAD_AXIS_LY,
    PAD_AXIS_RX,
    PAD_AXIS_RY,
    PAD_AXIS_COUNT,
};

/** 扳机索引（pad_state_t.trigger，保持模拟量）。 */
enum {
    PAD_TRIGGER_L = 0,
    PAD_TRIGGER_R,
    PAD_TRIGGER_COUNT,
};

/** 触摸板索引：PS 触摸板按左右两半上报，Steam 为两块触摸板。 */
enum {
    PAD_TOUCH_LEFT = 0,
    PAD_TOUCH_RIGHT,
    PAD_TOUCH_COUNT,
};

/** 能力位：这一帧里哪些字段真的来自设备（不置位表示该字段无意义）。 */
enum {
    PAD_CAP_MOTION = 1u << 0,
    PAD_CAP_TOUCHPAD = 1u << 1,
    PAD_CAP_TRIGGER_ANALOG = 1u << 2,
    PAD_CAP_BACK_BUTTONS = 1u << 3,
    PAD_CAP_MIC = 1u << 4,
    PAD_CAP_BATTERY = 1u << 5,
    PAD_CAP_RUMBLE = 1u << 6,
    /** 型号未识别，按 Xbox 布局兜底解析（结果仍可用，但字段可能错位）。 */
    PAD_CAP_FALLBACK_LAYOUT = 1u << 7,
};

typedef struct {
    bool present;
    bool pressed;
    uint16_t x;     /**< 0-4095（按设备分辨率归一）。 */
    uint16_t y;     /**< 0-4095。 */
    uint16_t raw_x; /**< 设备原始值，诊断与标定用。 */
    uint16_t raw_y;
} pad_touch_t;

typedef struct {
    bool present;
    /** 原始角速度与加速度（量程随家族不同，本轮不做物理单位归一）。 */
    int16_t gyro[3];
    int16_t accel[3];
    uint32_t timestamp_us;
} pad_motion_t;

/** 私有手柄状态：接收侧一帧的归一化结果。 */
typedef struct {
    uint32_t buttons;
    uint16_t axis[PAD_AXIS_COUNT];
    uint16_t trigger[PAD_TRIGGER_COUNT];
    pad_touch_t touch[PAD_TOUCH_COUNT];
    pad_motion_t motion;
    uint16_t mic_level; /**< 0-4095。 */
    bool mic_muted;
    uint8_t battery_percent; /**< 0-100，PAD_CAP_BATTERY 未置位时无意义。 */
    bool battery_present;
    bool charging;
    uint32_t caps;
    pad_family_t family;
    pad_conn_t conn;
    uint16_t vid;
    uint16_t pid;
    uint8_t report_id;
    uint8_t report_len;
    uint32_t seq; /**< 接收序号：丢帧统计与重复帧过滤。 */
} pad_state_t;

/**
 * 私有反馈格式（目标 → 输入设备方向）：主机下发的震动、玩家灯与触觉采样
 * 先归一到这里，具体投递由后续里程碑按目标设备能力实现。
 */
typedef struct {
    bool rumble_on[PAD_TRIGGER_COUNT];
    uint8_t rumble_strength[PAD_TRIGGER_COUNT]; /**< 0-255，归一强度。 */
    /** 目标侧原始参数包（NS2 为 2×16 字节 LRA 参数），供目标能力二次编码。 */
    uint8_t rumble_raw[PAD_TRIGGER_COUNT][16];
    uint8_t player_led; /**< 玩家灯掩码 bit0-3。 */
    bool haptic_sample_valid;
    uint8_t haptic_sample;
} pad_feedback_t;

/** 复位为静置默认：摇杆居中、无按键、无外设数据。 */
void pad_state_defaults(pad_state_t *state);

/** 复位为无反馈默认。 */
void pad_feedback_defaults(pad_feedback_t *feedback);

#ifdef __cplusplus
}
#endif
