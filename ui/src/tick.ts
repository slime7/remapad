/**
 * 帧节奏的公共来源：整份 UI 只从这里取「一帧等于多久」。
 *
 * 速率由 PocketJS 在构建期按 host profile（firmware/pocket.host.json）的
 * tickHz 烘进 bundle，真机 mount 时框架会断言它等于固件声明的 ui.__tickHz
 * （framework/src/host.ts 的 assertNativeHostContract），所以这里读到的值
 * 就是设备实际驱动的节奏。
 *
 * 时长（轮询间隔、提示停留、闪烁）写成毫秒经 ticksForMs 换算，帧步进（序列图
 * 里每张图停留几帧）经 ticksForStepAt60Hz 换算。实际帧数都由 TICK_HZ 算出，
 * 改 tick 频率只需改 profile，调用方的取值不用动。
 */
import { TICKS_PER_SECOND } from '@pocketjs/framework/vue-vapor/clock';

/** 每虚拟帧一次 UI turn（= host profile 的 tickHz）。 */
export const TICK_HZ = TICKS_PER_SECOND;

/** 一帧的毫秒数（30 Hz 下 33.33 ms）。 */
export const TICK_MS = 1000 / TICK_HZ;

/**
 * 毫秒 → 帧数。
 *
 * 结果可能不是整数：计时器（轮询间隔、提示停留）取整后用即可，帧步进见
 * ticksForStepAt60Hz。
 */
export function ticksForMs(ms: number): number {
  return ms / TICK_MS;
}

/** 帧数 → 毫秒。 */
export function msForTicks(ticks: number): number {
  return ticks * TICK_MS;
}

/** 帧步进基准取值的节奏。 */
const STEP_BASE_HZ = 60;

/**
 * 60 Hz 下的帧步进 → 当前 tick 频率下的帧数。
 *
 * 结果可能不是整数（60 Hz 的 3 帧在 30 Hz 下是 1.5 帧）：按帧推进的动画取整到
 * 整数帧（Math.round），保证每帧停留时长一致。
 */
export function ticksForStepAt60Hz(stepAt60Hz: number): number {
  return (stepAt60Hz * TICK_HZ) / STEP_BASE_HZ;
}

/** 一秒钟的虚拟帧数，供「每秒一次」的计数器与采样窗口共用。 */
export const SECOND_TICKS = Math.max(1, Math.round(ticksForMs(1000)));
