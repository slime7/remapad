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

#include "app_config.h"
#include "backlight.h"
#include "boot_splash.h"
#include "bridge/js_bridge.h"
#include "panel.h"
#include "touch.h"

#include "pocketjs/guest.h"
#include "pocketjs/guest_quickjs.h"
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
/** 持久化亮度缺失时的兜底值（app_config 加载后通常有用户设定值）。 */
#define REMAPAD_BACKLIGHT_PCT_DEFAULT 40

/* PocketJS 启动阶段：串口失败日志的标签与启动画面进度共用同一份顺序。 */
static const char *const REMAPAD_BOOT_STAGES[] = {
    "package_open",
    "package_select",
    "guest_create",
    "ui_core_create",
    "ui_qjs_create",
    "feed_pak",
    "mount",
    "native_bridge_surface",
    "guest_eval",
    "renderer_create",
    "target_create",
    "strip_buffer",
    "tick_hz",
};
#define REMAPAD_BOOT_STAGE_COUNT \
    (sizeof(REMAPAD_BOOT_STAGES) / sizeof(REMAPAD_BOOT_STAGES[0]))

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

/** 进入某个启动阶段：同步串口标签与启动画面进度，返回该阶段的日志标签。 */
static const char *boot_stage(size_t index)
{
    if (index >= REMAPAD_BOOT_STAGE_COUNT) {
        index = REMAPAD_BOOT_STAGE_COUNT - 1U;
    }
    boot_splash_progress((int)index + 1, (int)REMAPAD_BOOT_STAGE_COUNT);
    return REMAPAD_BOOT_STAGES[index];
}

/** 持久化亮度（0 视为未设置）：启动画面与首帧共用同一个取值。 */
static uint8_t effective_brightness(void)
{
    uint8_t brightness = app_config_get()->brightness;
    if (brightness == 0U) {
        brightness = REMAPAD_BACKLIGHT_PCT_DEFAULT;
    }
    return brightness;
}

/* 产品控制面入口：guest 侧 driver.ts 约定 globalThis.__nativeBridge.postMessage。
 * 只入队，不做任何重活；命令在 owner task 的每帧 js_bridge_service 里处理。 */
static JSValue native_bridge_post_message(JSContext *ctx, JSValueConst this_val,
                                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) {
        return JS_UNDEFINED;
    }
    size_t len = 0;
    const char *cmd = JS_ToCStringLen(ctx, &len, argv[0]);
    if (cmd == NULL) {
        return JS_UNDEFINED;
    }
    js_bridge_enqueue(cmd);
    JS_FreeCString(ctx, cmd);
    return JS_UNDEFINED;
}

static esp_err_t install_native_bridge_surface(JSContext *ctx, void *user_data)
{
    (void)user_data;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue bridge = JS_NewObject(ctx);
    JSValue fn = JS_NewCFunction(ctx, native_bridge_post_message, "postMessage", 1);
    /* SetPropertyStr 接管传入值与 global 的引用：bridge/fn 交给 bridge/global
     * 持有，这里只归还 global 自身的引用，不能重复 free bridge。 */
    JS_SetPropertyStr(ctx, bridge, "postMessage", fn);
    JS_SetPropertyStr(ctx, global, "__nativeBridge", bridge);
    JS_FreeValue(ctx, global);
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
        /* 首帧落屏即完成交接：释放启动画面缓冲，面板内容从此由 PocketJS
         * 的 damage 窗口维护。 */
        boot_splash_end();
        ESP_LOGI(TAG, "PocketJS UI ready, boot splash handed over");
        /* 背光通常已由启动画面点亮（持久化亮度，开机恒为亮屏态）；这里再设
         * 一次是面板可用但启动画面不可用时的兜底。 */
        esp_err_t backlight_result = backlight_set(effective_brightness());
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
    /* 面板就绪后立刻画启动画面并点亮背光：guest 的 mount/eval 要几秒钟，
     * 这段时间屏幕已经有内容，开机不再是黑屏等待。 */
    if (runtime->panel_ready) {
        const esp_err_t splash_result = boot_splash_begin(effective_brightness());
        if (splash_result != ESP_OK) {
            ESP_LOGW(TAG, "boot splash unavailable: %s", esp_err_to_name(splash_result));
        }
    }

    const char *stage = boot_stage(0);
    esp_err_t result = pocketjs_package_open(
        pocketjs_package_remapad.data,
        pocketjs_package_remapad.size,
        0,
        &runtime->package);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = boot_stage(1);
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

    stage = boot_stage(2);
    pocketjs_guest_config_t guest_config;
    pocketjs_guest_config_defaults(&guest_config);
    /* JS 堆预算：7 个常驻页面 + 5 键导航的 mount 峰值实测越过 4MB 默认
     * （guest_eval OOM），提到 5.5MB 后挂载峰值仍随页面增重间歇性越过
     * （QuickJS InternalError: out of memory，两次连续复现），再提到
     * 6.5MB；OOM 时 PSRAM 尚余 2.6MB、内部 RAM 尚余 264KB，该值留有
     * 运行期增长余量，继续扩页面前先看 mount 后的 js_heap 日志。 */
    guest_config.heap_limit = 6656U * 1024U;
    guest_config.stack_limit = REMAPAD_POCKETJS_STACK_LIMIT;
    guest_config.prefer_psram = true;
    result = pocketjs_guest_create(&guest_config, &runtime->guest);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = boot_stage(3);
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

    stage = boot_stage(4);
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

    stage = boot_stage(5);
    result = pocketjs_ui_qjs_feed_pak(
        runtime->binding,
        app.pak.data,
        app.pak.size);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = boot_stage(6);
    result = pocketjs_ui_qjs_mount(runtime->binding);
    if (result != ESP_OK) {
        goto fail;
    }
    /* 产品控制面 surface 必须在 guest eval 前安装（官方约定：surfaces 先于
     * eval），这样 bundle 初始化时 driver 就能看到 __nativeBridge。 */
    stage = boot_stage(7);
    result = pocketjs_guest_quickjs_install_once(
        runtime->guest,
        "remapad.native-bridge",
        install_native_bridge_surface,
        NULL);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = boot_stage(8);
    result = pocketjs_guest_eval(
        runtime->guest,
        (const char *)app.javascript.data,
        app.javascript.size - 1U,
        "remapad");
    if (result != ESP_OK) {
        goto fail;
    }
    /* 启动路径的 JS 堆峰值一次性采样（全堆遍历有毫秒级开销，不进周期日志）。 */
    pocketjs_guest_stats_t boot_stats = {.struct_size = sizeof(boot_stats)};
    if (pocketjs_guest_stats(runtime->guest, &boot_stats) == ESP_OK) {
        ESP_LOGI(TAG, "js heap after eval: used=%" PRIu32 "kB limit=%" PRIu32 "kB",
                 (uint32_t)(boot_stats.heap_used / 1024U),
                 (uint32_t)(boot_stats.heap_limit / 1024U));
    }

    stage = boot_stage(9);
    pocketjs_rgb565_renderer_config_t renderer_config;
    pocketjs_rgb565_renderer_config_defaults(&renderer_config);
    renderer_config.scale = pocketjs_package_remapad_contract.raster_density;
    result = pocketjs_rgb565_renderer_create(
        &renderer_config,
        &runtime->renderer);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = boot_stage(10);
    result = pocketjs_rgb565_target_create(&runtime->target);
    if (result != ESP_OK) {
        goto fail;
    }
    stage = boot_stage(11);
    result = allocate_strip_buffer(
        runtime,
        &pocketjs_package_remapad_contract);
    if (result != ESP_OK) {
        goto fail;
    }

    stage = boot_stage(12);
    runtime->tick_hz = pocketjs_ui_qjs_tick_hz(runtime->binding);
    if (runtime->tick_hz == 0U) {
        result = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    js_bridge_attach(runtime->guest);
    ESP_LOGI(TAG, "native bridge ready: __nativeBridge.postMessage -> js_bridge");

    return ESP_OK;

fail:
    /* UI 起不来时把启动画面留在屏上并把进度条标成错误色，避免直接黑屏。 */
    boot_splash_fail();
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

        /* 控制面命令处理与配对状态机驱动；同在 owner task，guest 事件经
         * eval 回发（render 之后调用不占用 turn 预算的统计）。 */
        js_bridge_service();

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

    esp_err_t bridge_result = js_bridge_init();
    if (bridge_result != ESP_OK) {
        ESP_LOGE(TAG, "js_bridge init failed: %s", esp_err_to_name(bridge_result));
        return bridge_result;
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
