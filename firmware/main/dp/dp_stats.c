#include "dp_stats.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/** 输入数据的老化上限：超过说明输入源已停发，这次发出不记延迟。 */
#define DP_STATS_INPUT_STALE_US 1000000LL
/** 速率窗长与读数上限。 */
#define DP_STATS_WINDOW_US 1000000LL
#define DP_STATS_HZ_MAX 9999
/** 延迟 EMA 的移位权重（1/8）：抖动平均掉，节拍切换也跟得上。 */
#define DP_STATS_EMA_SHIFT 3

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_input_count;
static volatile int64_t s_last_input_us;
/** 延迟 EMA（微秒定点，0 = 无读数）：数据面单写、界面只读，免锁。 */
static volatile int64_t s_age_ema_us;

/* 速率窗基线（界面轮询任务独用，不加锁）：锚定标记单独记，时间 0 是合法基线。 */
static bool s_window_anchored;
static uint32_t s_window_count;
static int64_t s_window_start_us;
static int s_window_hz;

void dp_stats_note_input_us(int64_t now_us)
{
  portENTER_CRITICAL(&s_mux);
  s_input_count++;
  s_last_input_us = now_us;
  portEXIT_CRITICAL(&s_mux);
}

void dp_stats_note_output_us(int64_t now_us)
{
  const int64_t last = s_last_input_us;
  if (last == 0) {
    return;
  }
  const int64_t age = now_us - last;
  if (age < 0 || age > DP_STATS_INPUT_STALE_US) {
    return;
  }
  const int64_t ema = s_age_ema_us;
  s_age_ema_us = ema == 0 ? age : ema + ((age - ema) >> DP_STATS_EMA_SHIFT);
}

int dp_stats_input_hz(void)
{
  return dp_stats_input_hz_at(esp_timer_get_time());
}

int dp_stats_input_hz_at(int64_t now_us)
{
  if (!s_window_anchored) {
    s_window_anchored = true;
    s_window_start_us = now_us;
    s_window_count = s_input_count;
    return 0;
  }
  const int64_t elapsed = now_us - s_window_start_us;
  if (elapsed < DP_STATS_WINDOW_US) {
    return s_window_hz;
  }
  /* 计数是无符号累计，回绕时差值为负就当 0 处理，不做负数速率。 */
  const int64_t delta = (int64_t)(uint32_t)(s_input_count - s_window_count);
  int hz = (int)(delta * 1000000LL / elapsed);
  if (hz < 0) {
    hz = 0;
  }
  if (hz > DP_STATS_HZ_MAX) {
    hz = DP_STATS_HZ_MAX;
  }
  s_window_start_us = now_us;
  s_window_count = s_input_count;
  s_window_hz = hz;
  return hz;
}

int dp_stats_latency_ms(void)
{
  const int64_t ema = s_age_ema_us;
  if (ema <= 0) {
    return 0;
  }
  return (int)((ema + 500) / 1000);
}

void dp_stats_reset(void)
{
  s_input_count = 0;
  s_last_input_us = 0;
  s_age_ema_us = 0;
  s_window_anchored = false;
  s_window_count = 0;
  s_window_start_us = 0;
  s_window_hz = 0;
}
