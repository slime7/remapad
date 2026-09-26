#include "battery.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery_curve.h"

static const char *TAG = "driver_battery";

/* 电池采样：B+ 经 R3 200K / R7 100K 分压接 BAT_ADC（GPIO1），
 * 因此 VBAT = VADC × 3（见 docs/hardware.md）。 */
#define BATTERY_ADC_GPIO 1
#define BATTERY_DIVIDER_FACTOR 3
#define BATTERY_ATTEN ADC_ATTEN_DB_12
#define BATTERY_BITWIDTH ADC_BITWIDTH_DEFAULT
#define BATTERY_SAMPLE_PERIOD_MS 250
/** 每轮过采样次数：ADC 单次读数有几个 LSB 抖动，分压源阻抗又偏高
 *  （200K∥100K ≈ 67K），取平均把抖动压到几毫伏。 */
#define BATTERY_SAMPLE_COUNT 16
/** 12 dB 衰减下的名义满量程，只在 eFuse 校准数据不可用时兜底。 */
#define BATTERY_NOMINAL_FULL_SCALE_MV 3100
#define BATTERY_FULL_SCALE_CODE 4095

/* 充电状态推断：ETA6098 的 STAT 引脚在板上空置（核对原理图确认）、没有引到
 * GPIO；板上也没有 VBUS 检测脚，因此「是否在充电」无法测量，只能看采样
 * 电压的趋势。充电时端电压抬高并持续上升，放电时随电量下降，恒压阶段
 * 与静置状态电压持平——持平不动时保持上一次结论。 */
#define BATTERY_TREND_INTERVAL_US (30 * 1000 * 1000LL)
#define BATTERY_TREND_SLOTS 8
#define BATTERY_TREND_THRESHOLD_MV 20

typedef struct {
  uint32_t voltage_mv;
  uint8_t percentage;
  bool charging;
} battery_state_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static battery_state_t s_state;

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_channel;
static adc_cali_handle_t s_cali;
static bool s_cali_ready;
static bool s_started;

static uint16_t s_trend[BATTERY_TREND_SLOTS];
static size_t s_trend_head;
static size_t s_trend_len;

/** 读取 ADC 引脚电压（毫伏）：多轮过采样取平均，全部失败时返回 false。 */
static bool sample_pin_mv(uint32_t *out_mv)
{
  uint32_t sum_mv = 0;
  uint32_t good = 0;
  for (uint32_t index = 0; index < BATTERY_SAMPLE_COUNT; index++) {
    int raw = 0;
    if (adc_oneshot_read(s_adc, s_channel, &raw) != ESP_OK) {
      continue;
    }
    int pin_mv = 0;
    if (s_cali_ready) {
      if (adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) != ESP_OK) {
        continue;
      }
    } else {
      pin_mv = raw * BATTERY_NOMINAL_FULL_SCALE_MV / BATTERY_FULL_SCALE_CODE;
    }
    sum_mv += (uint32_t)pin_mv;
    good++;
  }
  if (good == 0) {
    return false;
  }
  *out_mv = sum_mv / good;
  return true;
}

/** 记一格趋势样本并按窗口首尾电压差推断充电状态。 */
static bool update_charging_trend(uint16_t voltage_mv, bool previous)
{
  s_trend[s_trend_head] = voltage_mv;
  s_trend_head = (s_trend_head + 1) % BATTERY_TREND_SLOTS;
  if (s_trend_len < BATTERY_TREND_SLOTS) {
    s_trend_len++;
    return previous;
  }
  const uint16_t oldest = s_trend[s_trend_head];
  const uint16_t newest = s_trend[(s_trend_head + BATTERY_TREND_SLOTS - 1) % BATTERY_TREND_SLOTS];
  if (newest - oldest >= BATTERY_TREND_THRESHOLD_MV) {
    return true;
  }
  if (oldest - newest >= BATTERY_TREND_THRESHOLD_MV) {
    return false;
  }
  return previous;
}

static void battery_task(void *param)
{
  (void)param;
  uint32_t filtered_mv = 0;
  bool have_filtered = false;
  int64_t trend_due_us = esp_timer_get_time() + BATTERY_TREND_INTERVAL_US;
  bool charging = false;

  for (;;) {
    uint32_t pin_mv = 0;
    if (sample_pin_mv(&pin_mv)) {
      const uint32_t battery_mv = pin_mv * BATTERY_DIVIDER_FACTOR;
      /* 一阶低通（4 Hz 采样，时间常数约 1 秒）：压掉抖动又不拖慢趋势。 */
      filtered_mv = have_filtered ? (filtered_mv * 3 + battery_mv) / 4 : battery_mv;
      have_filtered = true;

      const int64_t now_us = esp_timer_get_time();
      if (now_us >= trend_due_us) {
        trend_due_us = now_us + BATTERY_TREND_INTERVAL_US;
        charging = update_charging_trend((uint16_t)filtered_mv, charging);
      }

      portENTER_CRITICAL(&s_state_lock);
      s_state.voltage_mv = filtered_mv;
      s_state.percentage = battery_percent_from_mv(filtered_mv);
      s_state.charging = charging;
      portEXIT_CRITICAL(&s_state_lock);
    }
    vTaskDelay(pdMS_TO_TICKS(BATTERY_SAMPLE_PERIOD_MS));
  }
}

esp_err_t battery_init(void)
{
  if (s_started) {
    return ESP_OK;
  }

  adc_unit_t unit = ADC_UNIT_1;
  adc_channel_t channel = ADC_CHANNEL_0;
  esp_err_t err = adc_oneshot_io_to_channel(BATTERY_ADC_GPIO, &unit, &channel);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d is not an ADC pad: %s", BATTERY_ADC_GPIO, esp_err_to_name(err));
    return err;
  }

  const adc_oneshot_unit_init_cfg_t unit_cfg = {
    .unit_id = unit,
  };
  err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "adc unit init failed: %s", esp_err_to_name(err));
    return err;
  }
  const adc_oneshot_chan_cfg_t chan_cfg = {
    .atten = BATTERY_ATTEN,
    .bitwidth = BATTERY_BITWIDTH,
  };
  err = adc_oneshot_config_channel(s_adc, channel, &chan_cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "adc channel config failed: %s", esp_err_to_name(err));
    return err;
  }
  s_channel = channel;

  /* 出厂校准数据（eFuse）缺失时退回名义满量程换算，采样照常进行。 */
  const adc_cali_curve_fitting_config_t cali_cfg = {
    .unit_id = unit,
    .chan = channel,
    .atten = BATTERY_ATTEN,
    .bitwidth = BATTERY_BITWIDTH,
  };
  if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
    s_cali_ready = true;
  } else {
    ESP_LOGW(TAG, "adc calibration unavailable, using nominal scale");
  }

  /* 先同步采一轮：采样任务起跑前 bridge / CLI 就能拿到合理读数。 */
  uint32_t pin_mv = 0;
  uint32_t initial_mv = 0;
  if (sample_pin_mv(&pin_mv)) {
    s_state.voltage_mv = pin_mv * BATTERY_DIVIDER_FACTOR;
    s_state.percentage = battery_percent_from_mv(s_state.voltage_mv);
    initial_mv = s_state.voltage_mv;
  }

  if (xTaskCreate(battery_task, "battery", 3072, NULL, 2, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_started = true;
  ESP_LOGI(TAG, "battery sampling on GPIO%d (adc unit %d ch %d, cali=%s, %umV)", BATTERY_ADC_GPIO, (int)unit,
           (int)channel, s_cali_ready ? "on" : "off", (unsigned)initial_mv);
  return ESP_OK;
}

uint32_t battery_get_voltage_mv(void)
{
  portENTER_CRITICAL(&s_state_lock);
  const uint32_t voltage_mv = s_state.voltage_mv;
  portEXIT_CRITICAL(&s_state_lock);
  return voltage_mv;
}

uint8_t battery_get_percentage(void)
{
  portENTER_CRITICAL(&s_state_lock);
  const uint8_t percentage = s_state.percentage;
  portEXIT_CRITICAL(&s_state_lock);
  return percentage;
}

bool battery_is_charging(void)
{
  portENTER_CRITICAL(&s_state_lock);
  const bool charging = s_state.charging;
  portEXIT_CRITICAL(&s_state_lock);
  return charging;
}
