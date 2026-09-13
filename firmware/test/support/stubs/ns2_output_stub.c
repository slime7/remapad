/**
 * ns2_output 的测试替身：dp_source 只调用 ns2_output_nfc_state() 取 NFC 状态，
 * 这里给一个可设置的值，避免为了一个 getter 拖进整个输出模块与其依赖。
 */
#include "ns2_output.h"

static uint8_t s_nfc_state;

uint8_t ns2_output_nfc_state(void)
{
    return s_nfc_state;
}

void host_test_set_nfc_state(uint8_t value)
{
    s_nfc_state = value;
}

