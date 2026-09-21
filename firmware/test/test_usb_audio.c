/**
 * UAC1 音频流 OUT 接口解析（usb/usb_audio_parse.c）主机端用例：用 DualSense Edge 的描述符样本
 * 钉住挑选规则——跳过音频控制接口、麦克风流与 HID 接口，只认 Type I 16 位 PCM、
 * 带等时 OUT 端点的非 0 备用设置。
 */
#include "host_test.h"

#include <string.h>

#include "usb_audio_parse.h"

static const uint8_t s_ds_edge_config[] = {
    /* 配置描述符：4 个接口、总线供电 500mA。 */
    0x09, 0x02, 0x8D, 0x00, 0x04, 0x01, 0x00, 0x80, 0xFA,
    /* 接口 0：音频控制。 */
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x0A, 0x24, 0x01, 0x00, 0x01, 0x49, 0x00, 0x02, 0x01, 0x02,
    /* 接口 1 alt 0：零带宽。 */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    /* 接口 1 alt 1：播放流，链到端子 01、PCM，4ch/16bit/48kHz，
     * 等时 OUT 端点 0x01、MPS 392（0x0188）。 */
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x0B, 0x24, 0x02, 0x01, 0x04, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
    0x09, 0x05, 0x01, 0x09, 0x88, 0x01, 0x04, 0x00, 0x00,
    /* 接口 2 alt 1：麦克风流，2ch、等时 IN 端点 0x82、MPS 196。 */
    0x09, 0x04, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x06, 0x01, 0x01, 0x00,
    0x0B, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xBB, 0x00,
    0x09, 0x05, 0x82, 0x05, 0xC4, 0x00, 0x04, 0x00, 0x00,
    /* 接口 3：HID（解析应整个跳过）。 */
    0x09, 0x04, 0x03, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x95, 0x01,
    0x07, 0x05, 0x84, 0x03, 0x40, 0x00, 0x04,
    0x07, 0x05, 0x03, 0x03, 0x40, 0x00, 0x06,
};

/** 播放流端点描述符在夹具里的起点（截断与改写用例按它定位）。 */
#define PLAYBACK_EP_OFF 64
/** 播放流 AS_GENERAL 里 wFormatTag 低字节的偏移。 */
#define PLAYBACK_FORMAT_TAG_OFF 51

static void finds_dualsense_playback_stream(void)
{
    usb_audio_as_out_t as;
    REQUIRE(usb_audio_find_as_out(s_ds_edge_config, sizeof(s_ds_edge_config), &as));
    CHECK_EQ(as.iface, 1);
    CHECK_EQ(as.alt, 1);
    CHECK_EQ(as.ep_addr, 0x01);
    CHECK_EQ(as.ep_mps, 392);
    CHECK_EQ(as.channels, 4);
    CHECK_EQ(as.subframe_size, 2);
    CHECK_EQ(as.bit_resolution, 16);
    CHECK_EQ(as.sample_rate_hz, 48000);
}

static void non_pcm_format_is_rejected(void)
{
    uint8_t cfg[sizeof(s_ds_edge_config)];
    memcpy(cfg, s_ds_edge_config, sizeof(cfg));
    /* wFormatTag 改成 0x0003（IEC61937 压缩流）：不该被当成可合成的通道。 */
    cfg[PLAYBACK_FORMAT_TAG_OFF] = 0x03;

    usb_audio_as_out_t as;
    CHECK(!usb_audio_find_as_out(cfg, sizeof(cfg), &as));
}

static void truncated_descriptor_is_rejected(void)
{
    usb_audio_as_out_t as;
    /* 截在播放流端点描述符中间：组里没有端点，不成候选。 */
    CHECK(!usb_audio_find_as_out(s_ds_edge_config, PLAYBACK_EP_OFF + 3, &as));
    /* 更短的碎屑同样只报找不到，不越界。 */
    CHECK(!usb_audio_find_as_out(s_ds_edge_config, 12, &as));
    CHECK(!usb_audio_find_as_out(NULL, sizeof(s_ds_edge_config), &as));
    CHECK(!usb_audio_find_as_out(s_ds_edge_config, sizeof(s_ds_edge_config), NULL));
}

HOST_TEST_SUITE(suite_usb_audio, "usb_audio",
                {"DualSense 描述符里挑出播放流（接口 1 alt 1，4ch/16bit/48k）",
                 finds_dualsense_playback_stream},
                {"非 PCM 格式不当作可合成通道", non_pcm_format_is_rejected},
                {"截断或空描述符只报找不到", truncated_descriptor_is_rejected});
