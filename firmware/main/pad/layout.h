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
} pad_stick_style_t;

/** 电量字节风格。 */
typedef enum {
    PAD_BATTERY_PS = 0, /**< 低四位是 0-10 档、bit4 表示充电中（DS3 / DS4 / DualSense）。 */
    PAD_BATTERY_NS2,    /**< bit0 外部供电、bit1 充电中、bits2-5 电量等级 0-9。 */
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

/**
 * 马达跟哪条频带：主机的震动流是连续包络（NS2 参数包低频给冲击、高频给
 * 纹理），每颗马达按 rumble_band 选自己跟的带，振幅再按 rumble_max 缩放。
 * 惯例是重击马达（DS5 大马达、NS1 低频马达、Xbox 左马达）跟低频、纹理
 * 马达（DS5 小马达、NS1 高频马达）跟高频；0 值 = 低频（旧行不填也是这个）。
 */
typedef enum {
    PAD_RUMBLE_LF = 0, /**< 低频带（rumble_strength）。 */
    PAD_RUMBLE_HF,     /**< 高频带（rumble_hf_strength）。 */
} pad_rumble_band_t;

/** 输出报告的收尾方式：字段写完之后的补字节动作。 */
typedef enum {
    PAD_OUT_FRAME_NONE = 0, /**< 写完即可发送（有线形态）。 */
    /** PS 蓝牙形态：末 4 字节是 CRC32（种子字节 0xA2 参与计算，小端），
     *  缺它时主机应声不认——手柄收下报告但一个动作都不做。 */
    PAD_OUT_FRAME_PS_BT,
} pad_out_frame_t;

/**
 * HD 触觉波形映射规则（布局行声明，映射在布局内完成）：NS 的震动参数是
 * 「波形描述」（每侧最多 3 个时序子帧，子帧按时间顺序各播 1/3 周期，每帧
 * 一条低频音与一条高频音的频率与振幅档位），不是马达信号；声明了 HD 通路
 * 的设备按这份规则把主机的波形重整为自己的 PCM——震动映到触觉音圈、采样
 * 提示音的发声段映到扬声器（PC 侧再折进音圈）。频率码已按 9 位 log2 刻度
 * 解成 Hz，落地时夹进各带的 [min, max]（0 回落缺省）。
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
} pad_hd_haptic_t;

/** HD 渲染结果：每侧一条按时间顺序播放的子帧序列（低频/高频各一个振荡器，
 *  增益 0-255）加一路扬声器音色。由布局行的 hd 规则从主机波形渲染出来，
 *  合成引擎与桥接 FEEDBACK 帧都吃这一份——落地规则只在固件里有一份，PC
 *  只做哑渲染。 */
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
 * 运动字段描述：一次性给出取样位置、样本数与轴映射。轴映射把来源轴归一到
 * 私有约定（X 右为正、Y 上为正、Z 朝屏幕外为正）：gyro_src / accel_src 的
 * 第 i 项是私有三轴第 i 路取来源的第几路（PAD_OFF_NONE 表示该路缺失），
 * 三项全零表示恒等映射。invert_mask 的 bit0-2 表示陀螺 X/Y/Z 取反、
 * bit3-5 表示加速 X/Y/Z 取反。
 */
typedef struct {
    uint8_t samples; /**< 一次报告里的样本数；0 按 1 处理。 */
    uint8_t stride;  /**< 相邻样本的字节步长；0 按 12 处理。 */
    uint8_t gyro_src[3];
    uint8_t accel_src[3];
    uint8_t invert_mask;
} pad_motion_layout_t;

/**
 * 输出（反馈）报告描述：把主机下发的震动 / 玩家灯编码成该设备能吃的输出
 * 报告（触觉采样不进输出报告，由板载蜂鸣器或丢弃处置）。presets 是发送前
 * 写入的常量字节（偏移 + 值，偏移 PAD_OFF_NONE 表示结束），用来点亮 DS4 的
 * flags 或 DualSense 的两个 valid_flag。震动的两路强度按 rumble_max 缩放后
 * 写进 rumble_off；玩家灯按 led_style 写掩码或 RGB。report_id 为 0 表示该
 * 设备没有可写的反馈通道。
 */
#define PAD_OUT_PRESET_MAX 8

typedef struct {
    uint8_t report_id;
    uint8_t len; /**< 输出报告总长度（含 Report ID 字节）。 */
    uint8_t presets[PAD_OUT_PRESET_MAX][2];
    uint8_t rumble_off[PAD_TRIGGER_COUNT];
    uint8_t rumble_max[PAD_TRIGGER_COUNT];
    /** 每颗马达跟的频带（pad_rumble_band_t）；未填按低频。 */
    uint8_t rumble_band[PAD_TRIGGER_COUNT];
    uint8_t led_mask_off;
    uint8_t led_rgb_off;
    uint8_t led_style; /**< pad_led_style_t。 */
    /** 音频触觉：设备带可驱动的 UAC 音频触觉通道（DualSense 的 4ch PCM，
     *  后两路直连左右触觉音圈）。USB 直插时震动改走板上合成，HID 震动字节
     *  让位；桥接路径（PC 持有音频接口）不受影响。蓝牙接入时它声明的是
     *  蓝牙私有触觉流（DualSense 的 0x32 报告），让位语义相同。触觉采样
     *  不进任何渲染通路（板载蜂鸣器发声 / 蓝牙桥接丢弃），与音频触觉标记
     *  无关。 */
    uint8_t audio_haptic;
    /** HD 触觉波形映射规则（ops 为 0 表示该设备没有 HD 通路）。 */
    pad_hd_haptic_t hd;
    uint8_t frame;     /**< pad_out_frame_t。 */
    /** PS 蓝牙形态的序号字节偏移（高半字节逐报递增、低半字节 tag 保持 0，
     *  内核 DS_OUTPUT_SEQ_NO 的语义）；0 表示没有序号字节——DualShock 4 的
     *  蓝牙报告头是静态的（b1 hw_control、b2 音频控制），DualSense 才有它。 */
    uint8_t seq_off;
    /** 玩家灯落地值：四项依次对应主机掩码 bit0-3（1P-4P），0 表示原样写主机
     *  掩码。DualSense 的五颗灯是一组固定模式（1P 中灯、2P 中加外），不能直写
     *  主机掩码，需要这张表。 */
    uint8_t led_mask_map[4];
} pad_output_layout_t;

/**
 * 家族布局表的一行：按（家族, Report ID, 连接方式, PID）定位字段偏移。偏移
 * 一律从收到的报告首字节起算（含 Report ID）。
 *
 * 同一组合下有多个型号时报 PID 分行（PS 系三种型号都报 0x01，但字段偏移各不
 * 相同）；布局相同的多个 PID 写在同一行的 pids 里；pids 为空表示该组合下所有
 * 型号共用这行。报告未带 PID（离线构造或旧帧）时不做型号过滤，取最先匹配的行。
 *
 * 各系列的行放在 pad/layouts/ 下，一族一个文件；加一个系列＝加一个文件并在
 * layout.c 的模块表里登记一行。偏移初值多取自公开资料，实机接线时用
 * `pc/remapadctl.py --dump` 抓包核对，偏差只影响布局文件，不影响上下游。
 */
typedef struct {
    pad_family_t family;
    /** 连接方式；PAD_CONN_UNKNOWN 表示有线与蓝牙共用这一行。 */
    pad_conn_t conn;
    uint8_t report_id;
    /** 该行适用的 PID，0 结尾；首元素为 0 表示不按型号过滤。 */
    uint16_t pids[4];
    uint8_t buttons_off;
    uint8_t buttons_bytes;
    /** 方向键帽子开关偏移；0xFF 表示方向键在按键位图里。 */
    uint8_t hat_off;
    uint8_t trigger_off[PAD_TRIGGER_COUNT];
    uint8_t stick_off[PAD_AXIS_COUNT];
    /** 第一个触点的起始偏移（PAD_OFF_NONE 表示该行没有触摸数据）：每个触点
     *  4 字节，首字节 bit7 为 0 表示这一路有触点（低 7 位是触点 ID），其余
     *  三字节是 12 位 X（低 8 位 + 高 4 位）与 12 位 Y（高 4 位 + 低 8 位），
     *  DS4 与 DualSense 同一套约定。DS4 的一帧带多份触摸历史（每份 1 字节
     *  时间戳 + 2 个触点，USB 三份、蓝牙四份），偏移取第一份；DualSense 只有
     *  一份。登记了它的行必须同时给 touch_max_x / touch_max_y：归一到 0-4095
     *  与左右半区判定都用这一对量程。 */
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

/** 布局模块：一个手柄系列的全部布局行（一族一个文件，见 pad/layouts/）。 */
typedef struct {
    const char *name;
    const pad_layout_t *rows;
    size_t row_count;
} pad_layout_module_t;

/** Xbox 家族的按键位（bit0-3 方向键、bit4 Menu、bit5 View、bit6/7 摇杆按下、
 *  bit8/9 肩键、bit10 西瓜键、bit11 分享键、bit12-15 面键）。未识别型号的兜底
 *  布局也用它，因此定义在 layout.c 与 layouts/xbox.c 共用。 */
extern const uint32_t pad_xbox_btn_map[16];

/** PS 家族（DS4 与 DualSense）共用的按键位序：方向键帽子开关在低四位、面键在
 *  高四位，肩键、Create/Options、摇杆按下、PS / 触摸板 / 静音与 Edge 背键依次
 *  排在第二、三字节。DS3 的方向键在按键位图里，自带一份。 */
extern const uint32_t pad_ps_btn_map[24];

/**
 * 按报告的设备标识找布局行：家族取报告里的值，为空时按 VID/PID 判定；家族、
 * Report ID、连接方式、PID 任一不符就换下一行。找不到返回 NULL，由调用方决定
 * 是否套用兜底布局。family 输出实际使用的家族（可能由 VID/PID 补出）。
 */
const pad_layout_t *pad_layout_find(const pad_report_t *report, pad_family_t *family);

/**
 * 反馈方向查表：主机反馈到达时手上只有设备标识（没有报告帧），这里按
 * VID/PID 与连接方式找布局行，再用行的 out 描述编码输出报告。report_id
 * 不参与匹配（反馈不依赖输入报告格式）。family 输出判定出的家族。
 */
const pad_layout_t *pad_layout_find_by_ids(uint16_t vid, uint16_t pid, pad_conn_t conn,
                                           pad_family_t *family);

/** 未识别型号的兜底布局：按 Xbox 有线解析，能力位由调用方标记。 */
const pad_layout_t *pad_layout_fallback(void);

#ifdef __cplusplus
}
#endif
