#pragma once

#include "target.h"

/**
 * NS2 目标（转换段）：编码 Pro Controller 2 的输入报文（0x05 / 0x09），
 * 设备对外只模拟这一种手柄；将来支持 NS1 时新增 target/ns1/。
 */
const pad_target_t *ns2_target_get(void);
