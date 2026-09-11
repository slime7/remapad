#include "pocketjs_host.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "backlight.h"
#include "panel.h"
#include "touch.h"

#include "pocketjs/guest.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs_package_remapad.h"

static const char *TAG = "remapad_pocketjs";

/* QuickJS 从创建 runtime 的那个 task 上取栈指针来计算守卫下限，所以创建、
 * mount、求值和每一帧 turn 必须共用同一个 task；否则守卫量的是别人的栈。
 * Vue Vapor 应用在 mount 期间每层嵌套要吃掉几十 KB 的 C 栈，内部 RAM 拿不出
 * 这么多连续空间，因此 owner task 的栈放在 PSRAM。 */
#define REMAPAD_POCKETJS_STACK_LIMIT (256U * 1024U)
#define REMAPAD_POCKETJS_TASK_STACK_BYTES (288U * 1024U)
#define REMAPAD_POCKETJS_TASK_NAME "remapad-pjs"
#define REMAPAD_POCKETJS_TASK_PRIORITY 5
#define REMAPAD_POCKETJS_MAX_LAG_US 500000
#define REMAPAD_POCKETJS_STOP_TIMEOUT_MS 5000
#define REMAPAD_BACKLIGHT_PCT 40

typedef struct {
    pocketjs_package_t *package;
    pocketjs_guest_t *guest;
    pocketjs_ui_core_t *core;
    pocketjs_ui_qjs_t *binding;
    pocketjs_rgb565_renderer_t *renderer;
    pocketjs_rgb565_target_t *target;
    TaskHandle_t task;
    SemaphoreHandle_t wake;
    SemaphoreHandle_t exited;
    atomic_bool stopping;
    uint32_t tick_hz;
    uint32_t frames;
    uint32_t max_frame_us;
    uint32_t max_turn_us;
    uint32_t max_render_us;
    uint64_t window_turn_us;
    uint64_t window_render_us;
    uint32_t window_frames;
    uint16_t *strip_buffer;
    size_t strip_capacity_pixels;
    bool first_frame_logged;
    bool panel_ready;
} remapad_pocketjs_runtime_t;

static remapad_pocketjs_runtime_t s_runtime;

static void release_resources(remapad_pocketjs_runtime_t *runtime)
{
    if (runtime->strip_buffer != NULL) {
        heap_caps_free(runtime->strip_buffer);
        runtime->strip_buffer = NULL;
    }
    if (runtime->target != NULL) {
        pocketjs_rgb565_target_destroy(runtime->target);
        runtime->target = NULL;
    }
    if (runtime->renderer != NULL) {
        pocketjs_rgb565_renderer_destroy(runtime->renderer);
        runtime->renderer = NULL;
    }
    if (runtime->guest != NULL) {
        pocketjs_guest_destroy(runtime->guest);
        runtime->guest = NULL;
    }
    if (runtime->binding != NULL) {
        pocketjs_ui_qjs_destroy(runtime->binding);
        runtime->binding = NULL;
    }
    if (runtime->core != NULL) {
        pocketjs_ui_core_destroy(runtime->core);
        runtime->core = NULL;
    }
    if (runtime->package != NULL) {
        pocketjs_package_close(runtime->package);
        runtime->package = NULL;
    }
}

static esp_err_t destroy_runtime(remapad_pocketjs_runtime_t *runtime)
{
    if (runtime->task != NULL) {
        atomic_store_explicit(&runtime->stopping, true, memory_order_relaxed);
        if (runtime->binding != NULL) {
            pocketjs_ui_qjs_interrupt(runtime->binding);
        }
        (void)xSemaphoreGive(runtime->wake);
        if (xSemaphoreTake(runtime->exited,
                           pdMS_TO_TICKS(REMAPAD_POCKETJS_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "PocketJS owner task did not stop in time");
            return ESP_ERR_TIMEOUT;
        }
        runtime->task = NULL;
    }
    if (runtime->wake != NULL) {
        vSemaphoreDelete(runtime->wake);
        runtime->wake = NULL;
    }
    if (runtime->exited != NULL) {
        vSemaphoreDelete(runtime->exited);
        runtime->exited = NULL;
    }
    release_resources(runtime);
    return ESP_OK;
}

static esp_err_t scaled_dimension(uint32_t logical, uint32_t scale, size_t *out)
{
    if (out == NULL || logical == 0U || scale == 0U ||
        (size_t)logical > SIZE_MAX / (size_t)scale) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out = (size_t)logical * (size_t)scale;
    return ESP_OK;
}

static esp_err_t sample_input(pocketjs_ui_input_t *input, void *user_data)
{
    (void)user_data;
    input->buttons = 0;
    input->analog_x = 0;
    input->analog_y = 0;
    input->touches = NULL;
    input->touch_count = 0;

    /* 触点 id 必须在同一按压期间保持稳定；CST816T 为单点触摸，恒用 0。 */
    static pocketjs_ui_touch_t ui_touches[POCKETJS_UI_MAX_TOUCHES];
    touch_contact_t contacts[POCKETJS_UI_MAX_TOUCHES];
    const size_t count = touch_sample(contacts, POCKETJS_UI_MAX_TOUCHES);
    for (size_t index = 0; index < count; ++index) {
        ui_touches[index].id = 0;
        ui_touches[index].x = contacts[index].x;
        ui_touches[index].y = contacts[index].y;
    }
    input->touches = ui_touches;
    input->touch_count = count;
    return ESP_OK;
}

static esp_err_t render_frame(const pocketjs_ui_frame_view_t *frame, void *user_data)
{
    remapad_pocketjs_runtime_t *runtime = user_data;
    pocketjs_rgb565_damage_plan_t plan = {
        .struct_size = sizeof(plan),
    };
    esp_err_t result = pocketjs_rgb565_prepare(
        runtime->renderer, runtime->target, frame, &plan);
    if (result != ESP_OK) {
        return result;
    }

    const uint32_t scale = frame->raster_density;
    size_t physical_width = 0;
    result = scaled_dimension(frame->logical_width, frame->raster_density, &physical_width);
    if (result != ESP_OK) {
        pocketjs_rgb565_abort(runtime->renderer, runtime->target);
        return result;
    }
    for (uint32_t index = 0; index < plan.region_count; ++index) {
        const pocketjs_rgb565_rect_t region = plan.regions[index];
        size_t region_height = 0;
        result = scaled_dimension(region.height, scale, &region_height);
        if (result != ESP_OK || region_height == 0U ||
            physical_width > SIZE_MAX / region_height) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        const int region_y = (int)(region.y * scale);
        const int region_x = (int)(region.x * scale);
        const int region_width = (int)(region.width * scale);
        if (region_width <= 0 || (size_t)region_width > physical_width) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t region_pixels = physical_width * region_height;
        if (region_pixels > runtime->strip_capacity_pixels) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        memset(runtime->strip_buffer, 0, region_pixels * sizeof(*runtime->strip_buffer));
        pocketjs_rgb565_render_stats_t stats = {
            .struct_size = sizeof(stats),
        };
        result = pocketjs_rgb565_render_strip(
            runtime->renderer,
            frame, runtime->strip_buffer, region_pixels, region, NULL, &stats);
        if (result != ESP_OK) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return result;
        }
        /* strip 每行按视口全宽布局，renderer 只写 region 横向区间；先按行
         * 搬移为紧凑布局，再只把这个区间提交面板，避免把区间外的清零像素
         * 当作黑色刷进画面。 */
        for (size_t row = 0; row < region_height; ++row) {
            memmove(runtime->strip_buffer + row * region_width,
                    runtime->strip_buffer + row * physical_width + region_x,
                    (size_t)region_width * sizeof(*runtime->strip_buffer));
        }
        /* 面板传输失败时放弃本帧事务。 */
        if (runtime->panel_ready) {
            result = panel_transfer(
                runtime->strip_buffer,
                region_x,
                region_y,
                region_width,
                (int)region_height);
            if (result != ESP_OK) {
                pocketjs_rgb565_abort(runtime->renderer, runtime->target);
                return result;
            }
        }
    }

    result = pocketjs_rgb565_commit(runtime->renderer, runtime->target, frame);
    if (result != ESP_OK) {
        pocketjs_rgb565_abort(runtime->renderer, runtime->target);
        return result;
    }
    if (!runtime->first_frame_logged) {
        ESP_LOGI(TAG,
                 "PocketJS frame ready: %" PRIu32 "x%" PRIu32 " @%" PRIu32
                 "x, damage regions=%" PRIu32,
                 frame->logical_width,
                 frame->logical_height,
                 frame->raster_density,
                 plan.region_count);
        runtime->first_frame_logged = true;
        /* 背光在首帧提交成功后点亮，避免开机时闪出未初始化的面板内容。 */
        esp_err_t backlight_result = backlight_set(REMAPAD_BACKLIGHT_PCT);
        if (backlight_result != ESP_OK) {
            ESP_LOGW(TAG, "backlight on failed: %s", esp_err_to_name(backlight_result));
        }
    }
    return result;
}

static esp_err_t allocate_strip_buffer(
    remapad_pocketjs_runtime_t *runtime,
    const pocketjs_package_host_contract_t *contract)
{
    size_t width = 0;
    size_t height = 0;
    if (scaled_dimension(contract->logical_width, contract->raster_density, &width) != ESP_OK ||
        scaled_dimension(contract->logical_height, contract->raster_density, &height) != ESP_OK ||
        width > SIZE_MAX / height) {
        return ESP_ERR_INVALID_SIZE;
    }

    runtime->strip_capacity_pixels = width * height;
    if (runtime->strip_capacity_pixels > SIZE_MAX / sizeof(uint16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t bytes = runtime->strip_capacity_pixels * sizeof(uint16_t);
    runtime->strip_buffer = heap_caps_aligned_alloc(
        16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (runtime->strip_buffer == NULL) {
        runtime->strip_buffer = heap_caps_aligned_alloc(
            16, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (runtime->strip_buffer == NULL) {
        runtime->strip_capacity_pixels = 0;
        return ESP_ERR_NO_MEM;
    }

    memset(runtime->strip_buffer, 0, bytes);
    return ESP_OK;
}

static esp_err_t remapad_pocketjs_init(remapad_pocketjs_runtime_t *runtime)
{
    /* 面板与触摸初始化失败不阻断启动：面板失败时退回纯渲染 bring-up，
     * 触摸失败时 sample_input 每帧返回零触点。 */
    runtime->panel_ready = false;
    esp_err_t bsp_result = panel_init();
    if (bsp_result != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(bsp_result));
    } else {
        runtime->panel_ready = true;
    }
    bsp_result = touch_init();
    if (bsp_result != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed: %s", esp_err_to_name(bsp_result));
    }
    bsp_result = backlight_init();
    if (bsp_result != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(bsp_result));
    }

    const char *stage = "package_open";
    esp_err_t result = pocketjs_package_open(
        pocketjs_package_remapad.data,
        pocketjs_package_remapad.size,
        0,
        &runtime->package);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "package_select";
    pocketjs_package_variant_t app = {
        .struct_size = sizeof(app),
    };
    result = pocketjs_package_select(
        runtime->package,
        &pocketjs_package_remapad_contract,
        &app);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "guest_create";
    pocketjs_guest_config_t guest_config;
    pocketjs_guest_config_defaults(&guest_config);
    guest_config.heap_limit = 4U * 1024U * 1024U;
    guest_config.stack_limit = REMAPAD_POCKETJS_STACK_LIMIT;
    guest_config.prefer_psram = true;
    result = pocketjs_guest_create(&guest_config, &runtime->guest);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "ui_core_create";
    pocketjs_ui_core_config_t core_config;
    pocketjs_ui_core_config_defaults(&core_config);
    core_config.logical_width = pocketjs_package_remapad_contract.logical_width;
    core_config.logical_height = pocketjs_package_remapad_contract.logical_height;
    core_config.raster_density = pocketjs_package_remapad_contract.raster_density;
    core_config.tick_hz = pocketjs_package_remapad_contract.tick_hz;
    result = pocketjs_ui_core_create(&core_config, &runtime->core);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "ui_qjs_create";
    const pocketjs_ui_qjs_config_t binding_config = {
        .struct_size = sizeof(binding_config),
        .target_id = pocketjs_package_remapad_contract.target_id,
        .host_abi = pocketjs_package_remapad_contract.host_abi,
    };
    result = pocketjs_ui_qjs_create(
        runtime->guest,
        runtime->core,
        &binding_config,
        &runtime->binding);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "feed_pak";
    result = pocketjs_ui_qjs_feed_pak(
        runtime->binding,
        app.pak.data,
        app.pak.size);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = "mount";
    result = pocketjs_ui_qjs_mount(runtime->binding);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = "guest_eval";
    result = pocketjs_guest_eval(
        runtime->guest,
        (const char *)app.javascript.data,
        app.javascript.size - 1U,
        "remapad");
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "renderer_create";
    pocketjs_rgb565_renderer_config_t renderer_config;
    pocketjs_rgb565_renderer_config_defaults(&renderer_config);
    renderer_config.scale = pocketjs_package_remapad_contract.raster_density;
    result = pocketjs_rgb565_renderer_create(
        &renderer_config,
        &runtime->renderer);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = "target_create";
    result = pocketjs_rgb565_target_create(&runtime->target);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = "strip_buffer";
    result = allocate_strip_buffer(
        runtime,
        &pocketjs_package_remapad_contract);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = "tick_hz";
    runtime->tick_hz = pocketjs_ui_qjs_tick_hz(runtime->binding);
    if (runtime->tick_hz == 0U) {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    return ESP_OK;

fail:
    ESP_LOGE(TAG,
             "start failed at %s: %s (internal=%u largest=%u psram=%u)", stage,
             esp_err_to_name(result),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    release_resources(runtime);
    return result;
}

static bool owner_wait_until(remapad_pocketjs_runtime_t *runtime, int64_t deadline)
{
    while (!atomic_load_explicit(&runtime->stopping, memory_order_relaxed)) {
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            return true;
        }
        if (remaining > 2000) {
            const TickType_t ticks = pdMS_TO_TICKS((uint32_t)(remaining / 1000));
            (void)xSemaphoreTake(runtime->wake, ticks > 1 ? ticks - 1 : 1);
        } else {
            taskYIELD();
        }
    }
    return false;
}

static void pocketjs_owner_task(void *opaque)
{
    remapad_pocketjs_runtime_t *runtime = opaque;
    if (remapad_pocketjs_init(runtime) != ESP_OK) {
        goto exit;
    }

    ESP_LOGI(TAG, "PocketJS owner task running at %" PRIu32 " Hz (stack %u bytes)",
             runtime->tick_hz, (unsigned)REMAPAD_POCKETJS_TASK_STACK_BYTES);

    const int64_t started = esp_timer_get_time();
    uint64_t tick = 0;
    int64_t report_due = started + INT64_C(5000000);
    while (!atomic_load_explicit(&runtime->stopping, memory_order_relaxed)) {
        const int64_t deadline =
            started + (int64_t)((tick * UINT64_C(1000000)) / runtime->tick_hz);
        if (!owner_wait_until(runtime, deadline)) {
            break;
        }
        const int64_t now = esp_timer_get_time();
        if (now - deadline > REMAPAD_POCKETJS_MAX_LAG_US) {
            const uint64_t current =
                (uint64_t)(now - started) * runtime->tick_hz / UINT64_C(1000000);
            if (current > tick) {
                tick = current;
            }
        }

        pocketjs_ui_input_t input = { .struct_size = sizeof(input) };
        esp_err_t result = sample_input(&input, runtime);
        pocketjs_ui_frame_view_t frame = { .struct_size = sizeof(frame) };
        const int64_t frame_started = esp_timer_get_time();
        if (result == ESP_OK) {
            result = pocketjs_ui_turn(runtime->binding, &input, &frame);
        }
        const uint32_t turn_us = (uint32_t)(esp_timer_get_time() - frame_started);
        const int64_t render_started = esp_timer_get_time();
        if (result == ESP_OK) {
            result = render_frame(&frame, runtime);
        }
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "PocketJS turn failed: %s", esp_err_to_name(result));
        }
        const uint32_t render_us = (uint32_t)(esp_timer_get_time() - render_started);
        const uint32_t frame_us = (uint32_t)(esp_timer_get_time() - frame_started);
        if (frame_us > runtime->max_frame_us) {
            runtime->max_frame_us = frame_us;
        }
        if (turn_us > runtime->max_turn_us) {
            runtime->max_turn_us = turn_us;
        }
        if (render_us > runtime->max_render_us) {
            runtime->max_render_us = render_us;
        }
        runtime->frames++;
        runtime->window_frames++;
        runtime->window_turn_us += turn_us;
        runtime->window_render_us += render_us;
        tick++;

        if (esp_timer_get_time() >= report_due) {
            ESP_LOGI(TAG,
                     "frames=%" PRIu32 " avg_turn_us=%" PRIu32
                     " avg_render_us=%" PRIu32 " max_turn_us=%" PRIu32
                     " max_render_us=%" PRIu32,
                     runtime->frames,
                     runtime->window_frames == 0U
                         ? 0U
                         : (uint32_t)(runtime->window_turn_us / runtime->window_frames),
                     runtime->window_frames == 0U
                         ? 0U
                         : (uint32_t)(runtime->window_render_us / runtime->window_frames),
                     runtime->max_turn_us, runtime->max_render_us);
            runtime->window_frames = 0U;
            runtime->window_turn_us = 0U;
            runtime->window_render_us = 0U;
            report_due += INT64_C(5000000);
        }
    }

exit:
    xSemaphoreGive(runtime->exited);
    vTaskDelete(NULL);
}

esp_err_t remapad_pocketjs_start(void)
{
    if (s_runtime.task != NULL || s_runtime.package != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.wake = xSemaphoreCreateBinary();
    s_runtime.exited = xSemaphoreCreateBinary();
    if (s_runtime.wake == NULL || s_runtime.exited == NULL) {
        if (s_runtime.wake != NULL) {
            vSemaphoreDelete(s_runtime.wake);
            s_runtime.wake = NULL;
        }
        if (s_runtime.exited != NULL) {
            vSemaphoreDelete(s_runtime.exited);
            s_runtime.exited = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    atomic_init(&s_runtime.stopping, false);
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        pocketjs_owner_task,
        REMAPAD_POCKETJS_TASK_NAME,
        REMAPAD_POCKETJS_TASK_STACK_BYTES,
        &s_runtime,
        REMAPAD_POCKETJS_TASK_PRIORITY,
        &s_runtime.task,
        tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        vSemaphoreDelete(s_runtime.wake);
        s_runtime.wake = NULL;
        vSemaphoreDelete(s_runtime.exited);
        s_runtime.exited = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
