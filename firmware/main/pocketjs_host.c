#include "pocketjs_host.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "pocketjs/guest.h"
#include "pocketjs/package.h"
#include "pocketjs/render_rgb565.h"
#include "pocketjs/runner.h"
#include "pocketjs/ui_core.h"
#include "pocketjs/ui_qjs.h"
#include "pocketjs_package_remapad.h"

static const char *TAG = "remapad_pocketjs";

typedef struct {
    pocketjs_package_t *package;
    pocketjs_guest_t *guest;
    pocketjs_ui_core_t *core;
    pocketjs_ui_qjs_t *binding;
    pocketjs_rgb565_renderer_t *renderer;
    pocketjs_rgb565_target_t *target;
    pocketjs_runner_t *runner;
    uint16_t *strip_buffer;
    size_t strip_capacity_pixels;
    bool first_frame_logged;
} remapad_pocketjs_runtime_t;

static remapad_pocketjs_runtime_t s_runtime;

static esp_err_t destroy_runtime(remapad_pocketjs_runtime_t *runtime)
{
    if (runtime->runner != NULL) {
        const esp_err_t result = pocketjs_runner_stop(runtime->runner);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop PocketJS runner: %s", esp_err_to_name(result));
            return result;
        }
        runtime->runner = NULL;
    }
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

esp_err_t remapad_pocketjs_start(void)
{
    if (s_runtime.runner != NULL || s_runtime.package != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = pocketjs_package_open(
        pocketjs_package_remapad.data,
        pocketjs_package_remapad.size,
        0,
        &s_runtime.package);
    if (result != ESP_OK) {
        goto fail;
    }

    pocketjs_package_variant_t app = {
        .struct_size = sizeof(app),
    };
    result = pocketjs_package_select(
        s_runtime.package,
        &pocketjs_package_remapad_contract,
        &app);
    if (result != ESP_OK) {
        goto fail;
    }

    pocketjs_guest_config_t guest_config;
    pocketjs_guest_config_defaults(&guest_config);
    guest_config.heap_limit = 4U * 1024U * 1024U;
    guest_config.prefer_psram = true;
    result = pocketjs_guest_create(&guest_config, &s_runtime.guest);
    if (result != ESP_OK) {
        goto fail;
    }

    pocketjs_ui_core_config_t core_config;
    pocketjs_ui_core_config_defaults(&core_config);
    core_config.logical_width = pocketjs_package_remapad_contract.logical_width;
    core_config.logical_height = pocketjs_package_remapad_contract.logical_height;
    core_config.raster_density = pocketjs_package_remapad_contract.raster_density;
    core_config.tick_hz = pocketjs_package_remapad_contract.tick_hz;
    result = pocketjs_ui_core_create(&core_config, &s_runtime.core);
    if (result != ESP_OK) {
        goto fail;
    }

    const pocketjs_ui_qjs_config_t binding_config = {
        .struct_size = sizeof(binding_config),
        .target_id = pocketjs_package_remapad_contract.target_id,
        .host_abi = pocketjs_package_remapad_contract.host_abi,
    };
    result = pocketjs_ui_qjs_create(
        s_runtime.guest,
        s_runtime.core,
        &binding_config,
        &s_runtime.binding);
    if (result != ESP_OK) {
        goto fail;
    }

    result = pocketjs_ui_qjs_feed_pak(
        s_runtime.binding,
        app.pak.data,
        app.pak.size);
    if (result != ESP_OK) {
        goto fail;
    }
    result = pocketjs_ui_qjs_mount(s_runtime.binding);
    if (result != ESP_OK) {
        goto fail;
    }
    result = pocketjs_guest_eval(
        s_runtime.guest,
        (const char *)app.javascript.data,
        app.javascript.size - 1U,
        "remapad");
    if (result != ESP_OK) {
        goto fail;
    }

    pocketjs_rgb565_renderer_config_t renderer_config;
    pocketjs_rgb565_renderer_config_defaults(&renderer_config);
    renderer_config.scale = pocketjs_package_remapad_contract.raster_density;
    result = pocketjs_rgb565_renderer_create(
        &renderer_config,
        &s_runtime.renderer);
    if (result != ESP_OK) {
        goto fail;
    }
    result = pocketjs_rgb565_target_create(&s_runtime.target);
    if (result != ESP_OK) {
        goto fail;
    }
    result = allocate_strip_buffer(
        &s_runtime,
        &pocketjs_package_remapad_contract);
    if (result != ESP_OK) {
        goto fail;
    }

    pocketjs_runner_config_t runner_config;
    pocketjs_runner_config_defaults(&runner_config);
    runner_config.task_name = "remapad-pjs";
    runner_config.task_stack_bytes = 32U * 1024U;
    runner_config.sample_input = sample_input;
    runner_config.after_turn = render_frame;
    runner_config.user_data = &s_runtime;
    result = pocketjs_runner_start(
        s_runtime.binding,
        &runner_config,
        &s_runtime.runner);
    if (result != ESP_OK) {
        goto fail;
    }

    return ESP_OK;

fail:
    (void)destroy_runtime(&s_runtime);
    return result;
}
