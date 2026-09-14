#pragma once

#include "target.h"

/**
 * NS2 目标（转换段）：Pro Controller 2 与 JoyCon 2 共用同一份编码实现，
 * 由 ns2_output 按会话身份分摊左右半边；将来支持 NS1 时新增 target/ns1/。
 */
const pad_target_t *ns2_target_get(void);
