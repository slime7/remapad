/**
 * NS2 命令帧构造（ns2_frames.c）：主机用命令帧的固定字段判断应答方向与
 * 是否受理，字段错位会让整条命令通路静默失效。
 */
#include "host_test.h"

#include <string.h>

#include "app_config.h"
#include "ns2_frames.h"
#include "ns2_state.h"

/* 由 firmware/test/support/stubs/app_config_stub.c 提供。 */
void host_test_set_app_config(const app_config_t *config);

static void response_header_fields(void)
{
    uint8_t header[NS2_FRAME_HEADER_LEN];
    ns2_frame_response_header(header, NS2_CMD_PAIRING, NS2_FRAME_TRANSPORT_BLE, 0x03);
    CHECK_EQ(header[0], NS2_CMD_PAIRING);
    CHECK_EQ(header[1], NS2_FRAME_DIR_RESPONSE);
    CHECK_EQ(header[2], NS2_FRAME_TRANSPORT_BLE);
    CHECK_EQ(header[3], 0x03);
    CHECK_EQ(header[4], 0x10);
    CHECK_EQ(header[5], NS2_FRAME_ACK_OK);
    CHECK_EQ(header[6], 0x00);
    CHECK_EQ(header[7], 0x00);
}

static void response_body_and_capacity(void)
{
    const uint8_t body[2] = {0xAA, 0x55};
    uint8_t frame[NS2_FRAME_HEADER_LEN + sizeof(body)];

    CHECK_EQ(ns2_frame_response(frame, sizeof(frame), NS2_CMD_VERSION, NS2_FRAME_TRANSPORT_BLE,
                                0x02, body, sizeof(body)),
             sizeof(frame));
    CHECK_BYTES(&frame[NS2_FRAME_HEADER_LEN], body, sizeof(body));
    CHECK_EQ(frame[0], NS2_CMD_VERSION);
    CHECK_EQ(frame[1], NS2_FRAME_DIR_RESPONSE);

    /* 容量差一个字节：整体拒绝，不能写半个帧。 */
    uint8_t small[NS2_FRAME_HEADER_LEN + sizeof(body)];
    memset(small, 0xEE, sizeof(small));
    CHECK_EQ(ns2_frame_response(small, sizeof(small) - 1u, NS2_CMD_VERSION,
                                NS2_FRAME_TRANSPORT_BLE, 0x02, body, sizeof(body)),
             0);
    for (size_t index = 0; index < sizeof(small); index++) {
        CHECK_EQ(small[index], 0xEE);
    }

    /* 无应答体：只有帧头。 */
    uint8_t header_only[NS2_FRAME_HEADER_LEN];
    CHECK_EQ(ns2_frame_response(header_only, sizeof(header_only), NS2_CMD_SPI_FLASH,
                                NS2_FRAME_TRANSPORT_USB, 0x00, NULL, 0),
             NS2_FRAME_HEADER_LEN);
}

static void version_body(void)
{
    app_config_t config;
    memset(&config, 0, sizeof(config));
    config.fw_version[0] = 1;
    config.fw_version[1] = 6;
    config.fw_version[2] = 1;
    config.brightness = 60;
    host_test_set_app_config(&config);

    uint8_t body[NS2_VERSION_BODY_LEN];

    ns2_body_version(body);
    CHECK_EQ(body[0], 1);
    CHECK_EQ(body[1], 6);
    CHECK_EQ(body[2], 1);
    CHECK_EQ(body[3], 0x02);
    CHECK_EQ(body[4], 0x0C);
    CHECK_EQ(body[5], 0x00);
    CHECK_EQ(body[6], 0x00);
    CHECK_EQ(body[7], 0x00);
    CHECK_EQ(body[8], 0xFF);
    CHECK_EQ(body[9], 0xFF);
    CHECK_EQ(body[10], 0xFF);
    CHECK_EQ(body[11], 0xFF);

    /* 版本取自持久化配置，假升级改动它以后应答要跟着变。 */
    config.fw_version[1] = 7;
    host_test_set_app_config(&config);
    ns2_body_version(body);
    CHECK_EQ(body[1], 7);
}

static void pairing_public_key(void)
{
    /* 配对指令里的固定公钥（controller.md §3.2）：LTK = A1 XOR B1，
     * 写错一位主机就配不上，因此逐字节固定在这里。 */
    static const uint8_t expected[NS2_PAIR_PUBKEY_LEN] = {
        0x5C, 0xF6, 0xEE, 0x79, 0x2C, 0xDF, 0x05, 0xE1,
        0xBA, 0x2B, 0x63, 0x25, 0xC4, 0x1A, 0x5F, 0x10};
    CHECK_BYTES(ns2_pair_pubkey_b1, expected, sizeof(expected));
}

HOST_TEST_SUITE(suite_ns2_frames, "ns2_frames",
                {"响应帧头字段", response_header_fields},
                {"应答体拼接与容量边界", response_body_and_capacity},
                {"0x10 版本应答体", version_body},
                {"配对公钥常量", pairing_public_key});
