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
#include "console_out.h"
#include "dp_ui.h"
#include "input_link.h"
#include "ota_session.h"
#include "panel.h"
#include "render_accel.h"
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
/** 持久化亮度缺失时的兜底值（app_config 加载后通常有用户设定值）。 */
#define REMAPAD_BACKLIGHT_PCT_DEFAULT 40
/** 内存对照日志的周期：5 秒统计窗口的个数，60 秒一行。JS_ComputeMemoryUsage
 *  是全堆遍历（毫秒级），不能进 5 秒窗口；对照 QuickJS 记账值与 PSRAM 余量
 *  是鉴别「JS 堆滞留 vs 原生 ui_core 增长」的唯一仪表（ui_core 分配不走
 *  QuickJS 上限，PSRAM 涨满时它 abort 重启）。 */
#define REMAPAD_MEM_REPORT_WINDOWS 12
/** strip 缓冲数量：渲染下一条时，前几条仍在被 DMA 读取。 */
#define REMAPAD_STRIP_BUFFER_COUNT 3
/** 单条 strip 的逻辑高度：整屏 damage 会被切成这个高度的条带。一条
 * 240 × 32 × 2 = 15 kB，三条共 45 kB，能稳定落进内部 RAM（PSRAM 的写入
 * 带宽会拖住渲染）；条带小一些更省内部 RAM。 */
#define REMAPAD_STRIP_ROWS 32
/** 面板单次提交的等待上限，整帧 240x280 在 80 MHz 下约 13.5 ms。 */
#define REMAPAD_PANEL_TRANSFER_TIMEOUT_MS 200
/** 行带粒度：与 strip 条高一致，damage 折成行带后按「绝对行 / 条高」编号。 */
#define REMAPAD_BAND_ROWS REMAPAD_STRIP_ROWS
/** 行带表上限：视口高度 / 条高，280 / 32 = 9 条（最后一条不足条高）。 */
#define REMAPAD_BAND_MAX 16
/** strip 缓冲的 DMA 对齐（面板驱动的约定值）。 */
#define REMAPAD_STRIP_ALIGN 64

/** 截图单块回传的等待上限：PC 侧没在读时让这次截图尽快失败，不留半张图。 */
#define REMAPAD_SHOT_TX_TIMEOUT_MS 200u

/** 截图请求标志：串口 CLI（input_link / 控制台任务）置位、owner task 消费。 */
static atomic_bool s_shot_requested;

/** 内存全景请求标志：同截图请求，跨任务只传一个比特。 */
static atomic_bool s_mem_requested;

/** 一次截图已回传的进度：字节偏移与分块数，供日志与失败诊断。 */
typedef struct {
    uint32_t sent;
    uint32_t chunks;
} remapad_shot_progress_t;


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

/* 与阶段表一一对应的预计耗时（毫秒）：启动画面的进度条按它把
 * 时间轴拉开，避免十几个阶段挤在同一格里或者长阶段停在原地。阶段耗时变化
 * 时只需调整这里的权重，进度条依旧由经过时间驱动。 */
static const uint32_t REMAPAD_BOOT_STAGE_MS[REMAPAD_BOOT_STAGE_COUNT] = {
    110,   /* package_open */
    5,     /* package_select */
    20,    /* guest_create */
    5,     /* ui_core_create */
    5,     /* ui_qjs_create */
    115,   /* feed_pak */
    5,     /* mount */
    5,     /* native_bridge_surface */
    16240, /* guest_eval */
    5,     /* renderer_create */
    5,     /* target_create */
    5,     /* strip_buffer */
    5,     /* tick_hz */
};

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
    uint64_t window_damage_px;
    uint16_t *strip_buffers[REMAPAD_STRIP_BUFFER_COUNT];
    size_t strip_capacity_pixels;
    uint32_t strip_slot;
    /** 每个 strip 槽上一次提交的完成序号；0 表示还没提交过（等待立即通过）。 */
    uint32_t strip_tokens[REMAPAD_STRIP_BUFFER_COUNT];
    /** 行带表：本帧 damage 折成的行带，画完一条清一条。 */
    bool band_pending[REMAPAD_BAND_MAX];
    int32_t band_x0[REMAPAD_BAND_MAX];
    int32_t band_x1[REMAPAD_BAND_MAX];
    bool first_frame_logged;
    bool panel_ready;
} remapad_pocketjs_runtime_t;

static remapad_pocketjs_runtime_t s_runtime;

static void release_resources(remapad_pocketjs_runtime_t *runtime)
{
    for (size_t index = 0; index < REMAPAD_STRIP_BUFFER_COUNT; ++index) {
        if (runtime->strip_buffers[index] != NULL) {
            heap_caps_free(runtime->strip_buffers[index]);
            runtime->strip_buffers[index] = NULL;
        }
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
    boot_splash_progress((int)index + 1);
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
    /* 手柄操控模式（组合键捕获）期间，数据面把十字键与圆圈键映射成
     * PocketJS 的按键位，UI 的方向键焦点移动与圆圈键确认由此驱动
     * （见 dp/dp_ui.h）；不在模式里恒为 0，触摸输入不受影响。 */
    input->buttons = dp_ui_buttons();
    input->analog_x = 0;
    input->analog_y = 0;
    input->touches = NULL;
    input->touch_count = 0;

    /* 触点 id 必须在同一按压期间保持稳定；CST816T 为单点触摸，恒用 0。
     * 息屏（背光关闭）期间整段跳过：画面不可见，触点只会误触看不见的
     * 控件，不再进 UI（亮屏由 PWR 键或命令承担，触摸不负责唤醒）。 */
    if (app_config_get()->screen_on) {
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
    }
    return ESP_OK;
}


/**
 * 请求一次实机截图：置位后由 owner task 在下一帧消费（它手上才有当前 frame）。
 * 串口 CLI 在别的任务上调用，这里只写一个原子标志，不做任何重活。
 */
void remapad_ui_request_shot(void)
{
    atomic_store_explicit(&s_shot_requested, true, memory_order_relaxed);
}

/**
 * 请求一次实时内存全景：JS_ComputeMemoryUsage 会遍历整堆，只能与 guest 同任务
 * 执行，跨任务只置标志位；owner task 在下一帧读取并打到控制台出口。
 */
void remapad_ui_request_mem(void)
{
    atomic_store_explicit(&s_mem_requested, true, memory_order_relaxed);
}

/**
 * 把一条已渲染行带的像素按 200 字节分块回传（截图通路）：strip 的行距是
 * 视口全宽，窗口窄于视口时逐行取窗口内的列。返回非 ESP_OK 表示这次截图
 * 放弃——PC 侧按偏移是否覆盖满判定，半张图不会被写成文件。
 */
static esp_err_t shot_stream_band(const uint16_t *strip, size_t physical_width, int band_x,
                                  size_t band_width, size_t band_height,
                                  remapad_shot_progress_t *progress)
{
    const size_t row_bytes = band_width * sizeof(uint16_t);
    for (size_t line = 0; line < band_height; ++line) {
        const uint8_t *row = (const uint8_t *)(strip + line * physical_width + (size_t)band_x);
        size_t done = 0;
        while (done < row_bytes) {
            size_t piece = row_bytes - done;
            if (piece > INPUT_FRAME_IMAGE_CHUNK_MAX) {
                piece = INPUT_FRAME_IMAGE_CHUNK_MAX;
            }
            const esp_err_t err = input_link_send_image_data(progress->sent, &row[done], piece,
                                                            REMAPAD_SHOT_TX_TIMEOUT_MS);
            if (err != ESP_OK) {
                return err;
            }
            progress->sent += (uint32_t)piece;
            progress->chunks++;
            done += piece;
        }
    }
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

    const pocketjs_rgb565_accelerator_t *accelerator = render_accel();

    /* 截图请求：这一帧按整屏渲染一遍，逐条带把像素回传（PC 侧另存 PNG）。
     * 链路没在跑（host 模式）时直接丢弃请求，别让它一直挂在标志位上。 */
    const bool shot = atomic_exchange_explicit(&s_shot_requested, false, memory_order_relaxed) &&
                      input_link_active();
    remapad_shot_progress_t shot_progress = {.sent = 0U, .chunks = 0U};
    uint32_t shot_total = 0U;
    uint32_t shot_height = 0U;
    bool shot_failed = false;

    /* damage 折成行带表：行带是渲染与提交的最小单位，一条行带内多个 region 的
     * 横向范围合并成一个区间。本帧的每条行带都在本帧画完——隔行刷新在滚动
     * 时留下相邻行带相差一帧的纵向错位，观感上不可接受，因此不做字段切分。
     * 切点落在绝对行网格上，同一块屏幕在连续帧里恒属同一条行带。 */
    const uint32_t logical_height = frame->logical_height;
    const uint32_t band_count =
        (logical_height + REMAPAD_BAND_ROWS - 1U) / REMAPAD_BAND_ROWS;
    if (band_count == 0U || band_count > REMAPAD_BAND_MAX) {
        pocketjs_rgb565_abort(runtime->renderer, runtime->target);
        return ESP_ERR_INVALID_SIZE;
    }
    /* 整屏重画（full_redraw）时按整屏补一条区域，不依赖渲染器是否给出 region。 */
    if (plan.region_count == 0U && plan.full_redraw) {
        plan.region_count = 1U;
        plan.regions[0] = (pocketjs_rgb565_rect_t){
            .x = 0U,
            .y = 0U,
            .width = frame->logical_width,
            .height = frame->logical_height,
        };
    }
    /* 截图要整幅画面：把 damage 计划换成整屏一条区域，让每条行带都渲染。 */
    if (shot) {
        plan.region_count = 1U;
        plan.regions[0] = (pocketjs_rgb565_rect_t){
            .x = 0U,
            .y = 0U,
            .width = frame->logical_width,
            .height = frame->logical_height,
        };
    }
    for (uint32_t index = 0; index < plan.region_count; ++index) {
        const pocketjs_rgb565_rect_t region = plan.regions[index];
        if (region.width == 0U || region.height == 0U) {
            continue;
        }
        const int32_t x0 = (int32_t)region.x;
        const int32_t x1 = (int32_t)(region.x + region.width);
        const uint32_t first_band = region.y / REMAPAD_BAND_ROWS;
        uint32_t last_band =
            (region.y + region.height - 1U) / REMAPAD_BAND_ROWS;
        if (last_band >= band_count) {
            last_band = band_count - 1U;
        }
        for (uint32_t band = first_band; band <= last_band; ++band) {
            if (runtime->band_pending[band]) {
                if (x0 < runtime->band_x0[band]) {
                    runtime->band_x0[band] = x0;
                }
                if (x1 > runtime->band_x1[band]) {
                    runtime->band_x1[band] = x1;
                }
            } else {
                runtime->band_pending[band] = true;
                runtime->band_x0[band] = x0;
                runtime->band_x1[band] = x1;
            }
        }
    }
    /* 截图先声明尺寸与像素格式，再逐条带回传；尺寸越界或链路没答应就放弃
     * 这次截图（本帧仍照常渲染并提交面板）。 */
    if (shot) {
        size_t physical_height = 0;
        if (scaled_dimension(logical_height, scale, &physical_height) != ESP_OK ||
            physical_width > UINT16_MAX || physical_height > UINT16_MAX ||
            physical_width * physical_height > UINT32_MAX / sizeof(uint16_t)) {
            ESP_LOGW(TAG, "screenshot size out of range: %ux%u", (unsigned)physical_width,
                     (unsigned)physical_height);
            shot_failed = true;
        } else {
            shot_height = (uint32_t)physical_height;
            shot_total = (uint32_t)(physical_width * physical_height * sizeof(uint16_t));
            const esp_err_t announce = input_link_send_image_info(
                (uint16_t)physical_width, (uint16_t)physical_height, REMAPAD_SHOT_TX_TIMEOUT_MS);
            if (announce != ESP_OK) {
                ESP_LOGW(TAG, "screenshot announce failed: %s", esp_err_to_name(announce));
                shot_failed = true;
            }
        }
    }
    /* 行带按绝对行序自上而下渲染并提交：整幅内容在一帧内写完，面板扫描与本帧
     * 写入之间只剩一个撕裂边界，不再有隔行留下的相邻行带错位。 */
    for (uint32_t band_index = 0; band_index < band_count; ++band_index) {
        if (!runtime->band_pending[band_index]) {
            continue;
        }
        const uint32_t band_y = band_index * REMAPAD_BAND_ROWS;
        uint32_t band_rows = logical_height - band_y;
        if (band_rows > REMAPAD_BAND_ROWS) {
            band_rows = REMAPAD_BAND_ROWS;
        }
        const pocketjs_rgb565_rect_t band = {
            .x = (uint32_t)runtime->band_x0[band_index],
            .y = band_y,
            .width = (uint32_t)(runtime->band_x1[band_index] -
                                runtime->band_x0[band_index]),
            .height = band_rows,
        };
        runtime->band_pending[band_index] = false;
        const int band_x = (int)(band.x * scale);
        size_t band_width = 0;
        result = scaled_dimension(band.width, scale, &band_width);
        if (result != ESP_OK || band_width == 0U ||
            (size_t)band_x + band_width > physical_width) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        size_t band_height = 0;
        result = scaled_dimension(band.height, scale, &band_height);
        if (result != ESP_OK || band_height == 0U ||
            physical_width > SIZE_MAX / band_height) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t band_pixels = physical_width * band_height;
        if (band_pixels > runtime->strip_capacity_pixels) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return ESP_ERR_INVALID_SIZE;
        }
        /* 本条渲染进空闲缓冲：先等这个槽上一次的传输结束。三条缓冲轮转，
         * 队列里最多留三笔在飞，前几笔 DMA 与当前渲染重叠。 */
        const size_t slot = runtime->strip_slot;
        uint16_t *strip = runtime->strip_buffers[slot];
        runtime->strip_slot = (slot + 1U) % REMAPAD_STRIP_BUFFER_COUNT;
        result = panel_wait_seq(runtime->strip_tokens[slot],
                                REMAPAD_PANEL_TRANSFER_TIMEOUT_MS);
        if (result != ESP_OK) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return result;
        }
        pocketjs_rgb565_render_stats_t stats = {
            .struct_size = sizeof(stats),
        };
        result = pocketjs_rgb565_render_strip(
            runtime->renderer,
            frame, strip, band_pixels, band, accelerator, &stats);
        if (result != ESP_OK) {
            pocketjs_rgb565_abort(runtime->renderer, runtime->target);
            return result;
        }
        /* 截图回传必须在面板传输之前：panel_transfer_async 会把缓冲原地改成
         * SPI 线序（大端），之后再读就不是 RGB565 小端了。 */
        if (shot && !shot_failed) {
            const esp_err_t stream_err = shot_stream_band(strip, physical_width, band_x,
                                                          band_width, band_height,
                                                          &shot_progress);
            if (stream_err != ESP_OK) {
                ESP_LOGW(TAG, "screenshot aborted at %u/%u bytes: %s",
                         (unsigned)shot_progress.sent, (unsigned)shot_total,
                         esp_err_to_name(stream_err));
                shot_failed = true;
            }
        }
        /* renderer 先把 region 覆盖的那块填成背景色，调用方不必预先清零。
         * strip 行距是视口全宽，窗口窄于视口时必须按行压成紧凑布局（x=0 的窗口同样要压），
         * 否则面板读到错行内容；压缩时目标地址恒不高于源地址，前向复制安全。 */
        if (band_width != physical_width) {
            /* 逐行前向压缩（目标地址恒不高于源地址），按 32 位成对搬运。 */
            for (size_t line = 0; line < band_height; ++line) {
                const uint16_t *source = strip + line * physical_width + band_x;
                uint16_t *target = strip + line * band_width;
                size_t column = 0;
                for (; column + 2U <= band_width; column += 2U) {
                    uint32_t pair = 0;
                    memcpy(&pair, source + column, sizeof(pair));
                    memcpy(target + column, &pair, sizeof(pair));
                }
                for (; column < band_width; ++column) {
                    target[column] = source[column];
                }
            }
        }
        /* 面板传输失败时放弃本帧事务。spi_master 按提交顺序完成事务，窗口
         * 命令因此不会与上一笔数据交叉；缓冲的复用由上面的按槽等待保证。 */
        if (runtime->panel_ready) {
            result = panel_transfer_async(strip, band_x, (int)(band.y * scale),
                                          (int)band_width, (int)band_height,
                                          &runtime->strip_tokens[slot]);
            if (result != ESP_OK) {
                pocketjs_rgb565_abort(runtime->renderer, runtime->target);
                return result;
            }
        }
        runtime->window_damage_px += (uint64_t)band_width * (uint64_t)band_height;
    }

    result = pocketjs_rgb565_commit(runtime->renderer, runtime->target, frame);
    if (result != ESP_OK) {
        pocketjs_rgb565_abort(runtime->renderer, runtime->target);
        return result;
    }
    if (shot && !shot_failed) {
        const esp_err_t end_err = input_link_send_image_end(shot_total, REMAPAD_SHOT_TX_TIMEOUT_MS);
        if (end_err != ESP_OK) {
            ESP_LOGW(TAG, "screenshot end frame failed: %s", esp_err_to_name(end_err));
        } else {
            ESP_LOGI(TAG, "screenshot sent: %" PRIu32 "x%" PRIu32 " %u bytes in %u chunks",
                     (uint32_t)physical_width, shot_height, (unsigned)shot_total,
                     (unsigned)shot_progress.chunks);
        }
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
        /* 首帧落屏说明 guest 起得来、渲染通路通：交给 OTA 会话作为回滚健康
         * 门槛的一半条件（另一半是稳定运行时长）。 */
        ota_session_notify_ui_ready();
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

    /* 条带缓冲按固定条高分配，而不是整屏：渲染写入目标是内存带宽的瓶颈，
     * 小条才能进内部 RAM（见 REMAPAD_STRIP_ROWS）。 */
    runtime->strip_capacity_pixels = width * REMAPAD_STRIP_ROWS * contract->raster_density;
    (void)height;
    if (runtime->strip_capacity_pixels > SIZE_MAX / sizeof(uint16_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t bytes = runtime->strip_capacity_pixels * sizeof(uint16_t);
    const char *where = "internal";
    for (size_t index = 0; index < REMAPAD_STRIP_BUFFER_COUNT; ++index) {
        /* strip 既是 renderer 的写入目标也是 EDMA 的读取源。滚动的整屏帧要写
         * 134 kB，PSRAM 的写带宽会把渲染卡在内存上，因此优先要内部 RAM，
         * 要不到时退回 PSRAM（功能不变，只是渲染变慢）。 */
        uint16_t *buffer = heap_caps_aligned_alloc(
            REMAPAD_STRIP_ALIGN, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
        if (buffer == NULL) {
            buffer = heap_caps_aligned_alloc(
                REMAPAD_STRIP_ALIGN, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            where = "psram";
        }
        if (buffer == NULL) {
            runtime->strip_capacity_pixels = 0;
            return ESP_ERR_NO_MEM;
        }
        memset(buffer, 0, bytes);
        runtime->strip_buffers[index] = buffer;
    }
    ESP_LOGI(TAG, "strip buffers: %u x %u bytes in %s, internal free=%u psram free=%u",
             (unsigned)REMAPAD_STRIP_BUFFER_COUNT, (unsigned)bytes, where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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
        const esp_err_t splash_result = boot_splash_begin(
            effective_brightness(), REMAPAD_BOOT_STAGE_MS, REMAPAD_BOOT_STAGE_COUNT);
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
    /* JS 堆预算走 CONFIG_POCKETJS_GUEST_HEAP_LIMIT（取值在 sdkconfig.defaults），
     * 7 个常驻页面挂载与 devtools 飞行记录仪的峰值都算在这份预算里。 */
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
    /* 加速回调是本机整数实现（S3 无 PPA），比通用软件光栅快，阈值因此放到
     * 最低：小到一行的字形与纹理也走加速路径，避免「先建掩码再整块回退」。 */
    renderer_config.min_fill_pixels = 1;
    renderer_config.min_blend_pixels = 1;
    renderer_config.min_srm_pixels = 1;
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

    /* 初始化到此结束：启动画面停表，缓冲在动画任务退出时释放，画面从此由
     * PocketJS 的 damage 窗口维护。 */
    boot_splash_end();
    ESP_LOGI(TAG, "PocketJS UI ready, boot splash handed over");

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

/** CLI 回复出口：控制台通道的两种后端（UART0 / USJ vfs）都允许跨任务写。 */
static void mem_line(const char *text, size_t len)
{
    console_out_write(text, len);
}

/**
 * 实时内存全景（串口 mem 命令的应答）：PSRAM / 内部堆余量与历史最低、QuickJS 记账与对象计数、
 * JS turn 峰值耗时，全部在 owner task 上现场读取。整份报告拼成一块一次写出，避免 PC 侧应答窗断在中间。
 */
static void mem_report_print(const remapad_pocketjs_runtime_t *runtime)
{
    char report[320];
    size_t used = 0;
    used += (size_t)snprintf(report, sizeof(report),
             "mem psram free=%u largest=%u min=%u internal free=%u largest=%u min=%u\r\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    if (runtime->guest == NULL) {
        used += (size_t)snprintf(&report[used], sizeof(report) - used, "mem js guest=off\r\n");
        mem_line(report, used);
        return;
    }
    pocketjs_guest_stats_t stats = {.struct_size = sizeof(stats)};
    const bool have_heap = pocketjs_guest_stats(runtime->guest, &stats) == ESP_OK;
    JSMemoryUsage usage = {0};
    size_t gc_threshold = 0U;
    if (have_heap) {
        JSRuntime *qjs_runtime =
            JS_GetRuntime(pocketjs_guest_quickjs_context(runtime->guest));
        JS_ComputeMemoryUsage(qjs_runtime, &usage);
        gc_threshold = JS_GetGCThreshold(qjs_runtime);
    }
    snprintf(&report[used], sizeof(report) - used,
             "mem js_heap=%u/%u kB gc_thresh=%u kB allocs=%u obj=%u prop=%u arr=%u "
             "turns=%u errors=%u max_turn_us=%u max_render_us=%u\r\n",
             (unsigned)(usage.malloc_size / 1024U),
             (unsigned)(stats.heap_limit / 1024U),
             (unsigned)(gc_threshold / 1024U),
             (unsigned)usage.malloc_count, (unsigned)usage.obj_count,
             (unsigned)usage.prop_count, (unsigned)usage.array_count,
             (unsigned)stats.frames, (unsigned)stats.frame_errors,
             (unsigned)runtime->max_turn_us, (unsigned)runtime->max_render_us);
    mem_line(report, strlen(report));
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
    uint32_t report_windows = 0;
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

        /* 串口 mem 命令：任意任务置位都安全，全堆遍历只能在这里消费。 */
        if (atomic_exchange_explicit(&s_mem_requested, false, memory_order_relaxed)) {
            mem_report_print(runtime);
        }

        if (esp_timer_get_time() >= report_due) {
            const uint32_t frames_in_window =
                runtime->window_frames == 0U ? 1U : runtime->window_frames;
            ESP_LOGI(TAG,
                     "frames=%" PRIu32 " avg_turn_us=%" PRIu32
                     " avg_render_us=%" PRIu32 " avg_damage_px=%" PRIu32
                     " max_turn_us=%" PRIu32 " max_render_us=%" PRIu32,
                     runtime->frames,
                     (uint32_t)(runtime->window_turn_us / frames_in_window),
                     (uint32_t)(runtime->window_render_us / frames_in_window),
                     (uint32_t)(runtime->window_damage_px / frames_in_window),
                     runtime->max_turn_us, runtime->max_render_us);
            runtime->window_frames = 0U;
            runtime->window_turn_us = 0U;
            runtime->window_render_us = 0U;
            runtime->window_damage_px = 0U;
            report_due += INT64_C(5000000);
            report_windows++;
            if (report_windows % REMAPAD_MEM_REPORT_WINDOWS == 0U) {
                pocketjs_guest_stats_t mem_stats = {.struct_size = sizeof(mem_stats)};
                const bool have_heap = pocketjs_guest_stats(runtime->guest, &mem_stats) == ESP_OK;
                /* 分桶观察：obj/prop 涨而 func/str 平是「成环待回收」的特征。 */
                JSMemoryUsage usage = {0};
                size_t gc_threshold = 0U;
                if (have_heap && runtime->guest != NULL) {
                    JSRuntime *qjs_runtime =
                        JS_GetRuntime(pocketjs_guest_quickjs_context(runtime->guest));
                    JS_ComputeMemoryUsage(qjs_runtime, &usage);
                    gc_threshold = JS_GetGCThreshold(qjs_runtime);
                }
                ESP_LOGI(TAG,
                         "mem: js_heap=%" PRIu32 "kB/%" PRIu32 "kB gc_thresh=%" PRIu32 "kB "
                         "psram_free=%u internal_free=%u "
                         "allocs=%u obj_n=%u prop_n=%u arr_n=%u",
                         (uint32_t)(usage.malloc_size / 1024U),
                         have_heap ? (uint32_t)(mem_stats.heap_limit / 1024U) : 0U,
                         (uint32_t)(gc_threshold / 1024U),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)usage.malloc_count,
                         (unsigned)usage.obj_count,
                         (unsigned)usage.prop_count,
                         (unsigned)usage.array_count);
            }
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
    atomic_init(&s_shot_requested, false);
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
