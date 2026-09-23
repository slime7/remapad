#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pad_device.h"
#include "pad_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 字段偏移缺省值：该布局没有这个字段。 */
#define PAD_OFF_NONE 0xFF

/** 摇杆原始格式。 */
typedef enum {
    PAD_STICK_U8 = 0, /**< 单字节，中心 0x80（PS、Steam 原生报告）。 */
    PAD_STICK_I16,    /**< 有符号 16 位小端，中心 0（Xbox）。 */
    PAD_STICK_U12,    /**< 12 位紧凑打包三字节（NS2 的 0x05 / 0x09 报文体）。 */
    PAD_STICK_U16,    /**< 无符号 16 位小端，中心 0x8000（Xbox 蓝牙、NS1 的 0x3F 报文）。 */
} pad_stick_style_t;

/** 扳机字段的宽度与刻度。 */
typedef enum {
    PAD_TRIGGER_U8 = 0, /**< 单字节 0-255（PS、XInput 与 NS1）。 */
    PAD_TRIGGER_U10,    /**< 16 位小端承载的 10 位值 0-1023（Xbox 蓝牙报告）。 */
} pad_trigger_style_t;

/** 方向键帽子开关的编号方式。 */
typedef enum {
    /** 0 为上、顺时针，8 及以上为松开（HID 常见形态，PS 系用这个）。 */
    PAD_HAT_0UP = 0,
    /** 1 为上、顺时针，0 为松开（Xbox 蓝牙报告的帽子字节）。 */
    PAD_HAT_1UP,
} pad_hat_style_t;

/** 电量字节风格。 */
typedef enum {
    PAD_BATTERY_PS = 0, /**< 低四位是 0-10 档、bit4 表示充电中（DS3 / DS4 / DualSense）。 */
    PAD_BATTERY_NS2,    /**< bit0 外部供电、bit1 充电中、bits2-5 电量等级 0-9。 */
    /** Switch 一代：高四位 0-9 档（8 即满、9 是满电后的缓升档）、bit0 充电中。 */
    PAD_BATTERY_NS1,
} pad_battery_style_t;

/** 耳机（3.5mm）状态字节的解读方式；零值表示该行没有这个字段。 */
typedef enum {
    PAD_HEADSET_NONE = 0, /**< 不解析（未登记或未核对偏移）。 */
    /** PS 系单字节：bit0 插入、bit1 带麦（DS4 / DualSense 的音频状态字节）。 */
    PAD_HEADSET_PS,
} pad_headset_style_t;

/** 玩家灯映射方式。 */
typedef enum {
    PAD_LED_NONE = 0,    /**< 设备没有可控灯。 */
    PAD_LED_PLAYER_MASK, /**< 主机玩家灯掩码写进候选字节。 */
    PAD_LED_LIGHTBAR,    /**< 灯条：掩码换算成一组颜色写进 RGB 三字节。 */
} pad_led_style_t;

/** 马达跟哪条频带：重击马达（大马达 / 低频马达）跟低频、纹理马达跟高频；0 值 = 低频。 */
typedef enum {
    PAD_RUMBLE_LF = 0, /**< 低频带（rumble_strength）。 */
    PAD_RUMBLE_HF,     /**< 高频带（rumble_hf_strength）。 */
} pad_rumble_band_t;

/** 输出报告的收尾方式：字段写完之后的补字节动作。 */
typedef enum {
    PAD_OUT_FRAME_NONE = 0, /**< 写完即可发送（有线形态）。 */
    /** PS 蓝牙形态：末 4 字节是 CRC32（种子字节 0xA2，小端），缺它时手柄不执行报告。 */
    PAD_OUT_FRAME_PS_BT,
} pad_out_frame_t;

/** 震动编码方式。 */
typedef enum {
    /** 每颗马达一个强度字节，按 rumble_max 缩放（PS、Xbox、XInput 都走这条）。 */
    PAD_RUMBLE_SCALAR = 0,
    /** Switch 一代：每侧 4 字节，高频与低频各带频率与振幅，直接吃主机的
     *  LRA 波形（频率与振幅各自编码，见 docs/controller-ns1.md）。 */
    PAD_RUMBLE_NS1_WAVE,
} pad_rumble_style_t;

/**
 * HD 触觉波形映射规则（布局行声明，映射在布局内完成）：NS 的震动是波形描述
 * （每侧最多 3 个时序子帧），按这份规则重整为目标设备的 PCM；频率已解成 Hz，
 * 落地时夹进各带的 [min, max]，0 回落缺省。完整规则与承载通路见 docs/controller-ps.md。
 */
typedef struct {
    uint8_t ops; /**< 参与合成的时序子帧数上限（NS2 波形规则为 3）；0 = 无 HD 通路。 */
    uint16_t rate_hz;   /**< 承载 PCM 的采样率（USB UAC 为 48000；蓝牙私有流 3000）。 */
    uint16_t amp_peak;  /**< 归一满幅（gain 255）的 PCM 峰值（合成引擎按它刻度）。 */
    uint16_t cycle_ms;  /**< 子帧序列的整周期：3 个子帧各播 cycle_ms/3。 */
    uint16_t lf_min_hz;
    uint16_t lf_max_hz;
    uint16_t lf_default_hz;
    uint16_t hf_min_hz;
    uint16_t hf_max_hz;
    uint16_t hf_default_hz;
    uint16_t pulse_hz; /**< 采样「强震」段铺在音圈上的频率。 */
    uint16_t beep_hz;  /**< 采样「发声」段铺在扬声器上的频率。 */
    /** 振幅增益（num/den，0 表示 1/1）：主机档位偏小，增益在写 FEEDBACK 帧之前落地，
     *  板载合成与 PC 哑渲染因此吃同一份数值。 */
    uint8_t gain_num;
    uint8_t gain_den;
} pad_hd_haptic_t;

/** HD 渲染结果：每侧一条按时间顺序播放的子帧序列（低频/高频各一个振荡器，增益 0-255）
 *  加一路扬声器音色；合成引擎与桥接 FEEDBACK 帧吃同一份。 */
#define PAD_HD_KEY_MAX 3 /**< 每侧子帧上限（NS2 波形规则为 3）。 */

typedef struct {
    uint8_t key_count[2]; /**< 各侧有效子帧数（不足的子帧按静默子帧播放）。 */
    struct {
        uint16_t lf_freq;
        uint8_t lf_gain;
        uint16_t hf_freq;
        uint8_t hf_gain;
    } key[2][PAD_HD_KEY_MAX];
    struct {
        uint16_t freq;
        uint8_t gain;
    } speaker;
} pad_hd_render_t;

/**
 * 运动字段描述：给出取样位置、样本数、轴映射与设备原始刻度。gyro_src / accel_src 的
 * 第 i 项是私有三轴第 i 路取来源的第几路（PAD_OFF_NONE 表示缺失，三项全零 = 恒等映射）；
 * invert_mask 的 bit0-2 取反陀螺 XYZ、bit3-5 取反加速 XYZ；原始刻度由 accel_per_g /
 * gyro_per_dps_x1000 声明，解析段按它换算到 pad_state.h 的统一刻度。
 */
typedef struct {
    uint8_t samples; /**< 一次报告里的样本数；0 按 1 处理。 */
    uint8_t stride;  /**< 相邻样本的字节步长；0 按 12 处理。 */
    /** 样本前 6 字节是加速度（Switch 一代的 6 轴样本：加速在前、陀螺在后）。 */
    bool accel_first;
    uint8_t gyro_src[3];
    uint8_t accel_src[3];
    uint8_t invert_mask;
    /** 设备原始刻度：加速计数每 g（DS4 / DualSense 为 8192）；等于统一刻度
     *  （PAD_MOTION_ACCEL_PER_G）或 0 表示原值已是统一刻度、不做换算。 */
    uint16_t accel_per_g;
    /** 设备原始刻度：陀螺计数每 °/s 的一千倍（DS4 / DualSense 为 16000，即 16 计数每 °/s）；
     *  等于统一刻度（PAD_MOTION_GYRO_PER_DPS_X1000）或 0 表示不换算。 */
    uint16_t gyro_per_dps_x1000;
} pad_motion_layout_t;

/**
 * 输出（反馈）报告描述：把主机下发的震动 / 玩家灯编码成该设备的输出报告
 * （触觉采样不进输出报告）。presets 是发送前写入的常量字节（偏移 + 值，
 * 偏移 PAD_OFF_NONE 表示结束）；震动强度按 rumble_max 缩放后写进 rumble_off，
 * 玩家灯按 led_style 写掩码或 RGB；report_id 为 0 表示没有可写的反馈通道。
 */
#define PAD_OUT_PRESET_MAX 8

typedef struct {
    uint8_t report_id;
    uint8_t len; /**< 输出报告总长度（含 Report ID 字节）。 */
    /** 报告不带 Report ID 字节（XInput / Xbox 360 形态）：len 只数报文体，
     *  字段偏移从报文体首字节起算，编码结果原样写 OUT 端点。 */
    bool no_report_id;
    uint8_t presets[PAD_OUT_PRESET_MAX][2];
    /** 音频触觉让位期间的预置字节（与 presets 同格式；首槽偏移为 0 表示未声明，回落 presets）。
     *  音频接手时写回的报告只该带玩家灯，位段语义见 docs/controller-ps.md。 */
    uint8_t quiet_presets[PAD_OUT_PRESET_MAX][2];
    uint8_t rumble_off[PAD_TRIGGER_COUNT];
    uint8_t rumble_max[PAD_TRIGGER_COUNT];
    /** 每颗马达跟的频带（pad_rumble_band_t）；未填按低频。 */
    uint8_t rumble_band[PAD_TRIGGER_COUNT];
    /** 震动编码方式（pad_rumble_style_t）；PAD_RUMBLE_NS1_WAVE 时 rumble_off
     *  是每侧 4 字节块的块首，rumble_max 与 rumble_band 不参与（振幅自带刻度）。 */
    uint8_t rumble_style;
    uint8_t led_mask_off;
    uint8_t led_rgb_off;
    uint8_t led_style; /**< pad_led_style_t。 */
    /** 音频触觉：设备带可驱动的音频触觉通道（DualSense 的 4ch PCM 或蓝牙私有流）。
     *  USB 直插时震动改走板上合成、HID 震动字节让位；桥接路径（PC 持有音频接口）不受影响。 */
    uint8_t audio_haptic;
    /** HD 触觉波形映射规则（ops 为 0 表示该设备没有 HD 通路）。 */
    pad_hd_haptic_t hd;
    uint8_t frame;     /**< pad_out_frame_t。 */
    /** PS 蓝牙形态的序号字节偏移（高半字节逐报递增、低半字节 tag 保持 0）；0 表示没有序号字节。 */
    uint8_t seq_off;
    /** 玩家灯落地值：四项依次对应主机掩码 bit0-3（1P-4P），0 表示原样写主机掩码。 */
    uint8_t led_mask_map[4];
} pad_output_layout_t;

/**
 * 家族布局表的一行：按（家族, Report ID, 连接方式, PID, 报告长度）定位字段偏移，
 * 偏移一律从报告首字节起算（含 Report ID）。同一组合下多个型号报同一 Report ID 时按
 * PID 分行，布局相同的多个 PID 写在同一行，pids 为空表示该组合共用这行；
 * 报告未带 PID 时取最先匹配的行，因此限定长度的行排在通用的行前面。
 * 各系列的行放在 pad/layouts/ 下，一族一个文件，加一个系列再加一行登记。
 * 字段偏移的来源与核对状态见 docs/controller-xbox.md / controller-xinput.md /
 * controller-ns1.md / controller-ps.md。
 */
typedef struct {
    pad_family_t family;
    /** 连接方式；PAD_CONN_UNKNOWN 表示有线与蓝牙共用这一行。 */
    pad_conn_t conn;
    uint8_t report_id;
    /** 该行适用的 PID，0 结尾；首元素为 0 表示不按型号过滤。 */
    uint16_t pids[4];
    /** 报告长度过滤（含 Report ID 字节）：两个都为 0 表示不限长度；同一 Report ID
     *  下按报文长度分行的形态（精英背键位随报文长度变）靠它区分。 */
    uint8_t len_min;
    uint8_t len_max;
    uint8_t buttons_off;
    uint8_t buttons_bytes;
    /** 背键（精英手柄的 P1-P4、Edge 背键一类）：字节偏移 + 按位索引的位映射
     *  （最多 8 位），PAD_OFF_NONE 表示该行没有背键；back_mode_off 指向
     *  「背键已交给手柄内部配置档」的判定字节，它非零时背键位不采信。 */
    uint8_t back_off;
    uint8_t back_mode_off;
    const uint32_t *back_map;
    /** 方向键帽子开关偏移与编号方式（pad_hat_style_t）；0xFF 表示方向键在按键位图里。 */
    uint8_t hat_off;
    uint8_t hat_style;
    uint8_t trigger_off[PAD_TRIGGER_COUNT];
    uint8_t trigger_style; /**< pad_trigger_style_t。 */
    /** 数字扳机（NS 的 ZL / ZR 只有位）：每路触发器给位所在的字节偏移与位号，
     *  偏移为 PAD_OFF_NONE 或 0（报告 ID 字节）表示没有这一位；命中时按满量程填。 */
    uint8_t trigger_btn_off[PAD_TRIGGER_COUNT];
    uint8_t trigger_btn_bit[PAD_TRIGGER_COUNT];
    uint8_t stick_off[PAD_AXIS_COUNT];
    /** 第一个触点的起始偏移（PAD_OFF_NONE 表示该行没有触摸数据）：每点 4 字节，
     *  首字节 bit7 为 0 表示有触点（低 7 位是触点 ID），其余三字节是 12 位 X 与 12 位 Y；
     *  DS4 一帧带多份触摸历史、取第一份。登记了它的行必须同时给 touch_max_x / touch_max_y。 */
    uint8_t touch_off;
    uint8_t motion_off;
    uint8_t battery_off;
    /** 耳机状态字节偏移；读法与 headset_style 配套，PAD_OFF_NONE 表示未登记。 */
    uint8_t headset_off;
    uint16_t touch_max_x;
    uint16_t touch_max_y;
    pad_stick_style_t stick_style;
    /** 电量字节风格；PAD_CAP_BATTERY 未置位时不参与解析。 */
    pad_battery_style_t battery_style;
    /** 耳机状态读法；PAD_HEADSET_NONE（默认）时 headset_off 不参与解析。 */
    pad_headset_style_t headset_style;
    uint32_t caps;
    /** 设备 Y 轴向下为正时置位，解析侧翻成「上为正」。 */
    bool invert_y;
    const uint32_t *btn_map;
    /** 运动字段描述；motion_off 为 PAD_OFF_NONE 表示该型号没有运动数据。 */
    pad_motion_layout_t motion;
    /** 输出（反馈）报告描述；report_id 为 0 表示没有可写的反馈通道。 */
    pad_output_layout_t out;
    /** 设备自带报告语言（pad_lang_t）与期望的目标身份（pad_identity_t）。 */
    uint8_t native_lang;
    uint8_t native_identity;
} pad_layout_t;

/** 家族识别表项：该型号（VID:PID）归入模块的家族；pid 为 0 表示该 VID 下所有
 *  型号都归本家族，用于厂商 VID 分不开布局的第三方手柄（XInput 形态的第三方手柄）。 */
typedef struct {
    uint16_t vid;
    uint16_t pid;
} pad_id_t;

/** 布局模块：一个手柄系列的全部布局行与家族识别表（一族一个文件，见 pad/layouts/）。 */
typedef struct {
    const char *name;
    const pad_layout_t *rows;
    size_t row_count;
    /** 型号表：报告里的 VID:PID 命中即归入本模块的家族；未登记型号仍按 VID 判定。 */
    const pad_id_t *ids;
    size_t id_count;
} pad_layout_module_t;

/** Xbox 与 XInput 形态共用的按键位（bit0-3 方向键、bit4 Menu、bit5 View、bit6/7 摇杆按下、
 *  bit8/9 肩键、bit10 西瓜键、bit11 分享键、bit12-15 面键）。未识别型号的兜底
 *  布局也用它，因此定义在 layout.c 与 layouts/xbox.c / layouts/xinput.c 共用。 */
extern const uint32_t pad_xbox_btn_map[16];

/** PS 家族（DS4 与 DualSense）共用的按键位序：方向键帽子开关在低四位、面键在高四位，
 *  肩键、Create/Options、摇杆按下、PS / 触摸板 / 静音与 Edge 背键依次排在第二、三字节。 */
extern const uint32_t pad_ps_btn_map[24];

/**
 * 按报告的设备标识找布局行：家族取报告里的值，为空时按 VID/PID 判定；家族、
 * Report ID、连接方式、PID、报告长度任一不符就换下一行。找不到返回 NULL，由调用方决定
 * 是否套用兜底布局。family 输出实际使用的家族（可能由 VID/PID 补出）。
 */
const pad_layout_t *pad_layout_find(const pad_report_t *report, pad_family_t *family);

/**
 * 按型号定家族：厂商 VID 不足以判定家族的第三方手柄（例如各家厂商的 XInput
 * 形态手柄）走各模块的型号表，命中返回模块的家族；没有命中返回 PAD_FAMILY_UNKNOWN。
 */
pad_family_t pad_layout_family_from_ids(uint16_t vid, uint16_t pid);

/**
 * 反馈方向查表：主机反馈到达时手上只有设备标识（没有报告帧），这里按
 * VID/PID 与连接方式找布局行，再用行的 out 描述编码输出报告。report_id
 * 不参与匹配（反馈不依赖输入报告格式）。family 输出判定出的家族。
 */
const pad_layout_t *pad_layout_find_by_ids(uint16_t vid, uint16_t pid, pad_conn_t conn,
                                           pad_family_t *family);

/** 未识别型号的兜底布局：按 XInput 形态（Xbox 360 报文）解析，能力位由调用方标记。 */
const pad_layout_t *pad_layout_fallback(void);

#ifdef __cplusplus
}
#endif
