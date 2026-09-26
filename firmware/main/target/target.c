#include "target.h"

#include <stddef.h>

#include "battery_curve.h"

static const pad_target_t *s_target;
static bool s_relay_enabled = true;

void target_set(const pad_target_t *target)
{
  s_target = target;
}

const pad_target_t *target_get(void)
{
  return s_target;
}

const char *target_name(void)
{
  return s_target != NULL ? s_target->name : "none";
}

void target_set_facts(const pad_target_facts_t *facts)
{
  if (s_target != NULL && s_target->set_facts != NULL && facts != NULL) {
    s_target->set_facts(facts);
  }
}

void target_apply_pad_battery(pad_target_facts_t *facts, const pad_state_t *pad)
{
  if (pad == NULL || (pad->caps & PAD_CAP_BATTERY) == 0u || !pad->battery_present) {
    return;
  }
  facts->battery_level = battery_ns2_level_from_percent(pad->battery_percent);
  /* 0x05 报文的端电压字段没有真实来源（输入设备的电量以档位到达），按
     * 电压—容量表反演名义值，避免把板载电压漏给主机。 */
  facts->battery_mv = (uint16_t)battery_mv_from_percent(pad->battery_percent);
  facts->charging = pad->charging;
  facts->external_power = pad->charging;
}

void target_send_pad(const pad_state_t *pad)
{
  if (s_target == NULL || pad == NULL) {
    return;
  }
  /* 同代透传：设备自带语言与目标一致、且目标确认收下时不再重新编码。 */
  if (s_relay_enabled && s_target->send_raw != NULL && pad->native_lang != PAD_LANG_NONE &&
      pad->native_lang == s_target->language && s_target->send_raw(pad)) {
    return;
  }
  if (s_target->send_pad != NULL) {
    s_target->send_pad(pad);
  }
}

uint8_t target_language(void)
{
  return s_target != NULL ? s_target->language : PAD_LANG_NONE;
}

void target_set_relay(bool enabled)
{
  s_relay_enabled = enabled;
}

bool target_relay_enabled(void)
{
  return s_relay_enabled;
}
