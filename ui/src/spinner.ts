import { ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { ticksForStepAt60Hz } from './tick';

/** Braille 盲文点阵 Loading 字符序列：在同一位置依次播放。 */
export const SPINNER_CHARS = ['⠁', '⠂', '⠄', '⡀', '⢀', '⠠', '⠐', '⠈'] as const;

/** 每张序列图的步进基准（60 Hz 下的 frameStep）：8 帧一圈 0.4 秒。 */
const SPINNER_STEP_AT_60HZ = 3;

/** 当前 tick 下的整数帧步进（30 Hz 下为 2 帧）。 */
export const SPINNER_STEP = Math.max(
  1,
  Math.round(ticksForStepAt60Hz(SPINNER_STEP_AT_60HZ)),
);

/** 创建 Braille 文本帧动画响应式引用。 */
export function createBrailleSpinner() {
  const char = ref<string>(SPINNER_CHARS[0]);
  let tickCount = 0;
  let frameIndex = 0;

  onFrame(() => {
    tickCount++;
    if (tickCount >= SPINNER_STEP) {
      tickCount = 0;
      frameIndex = (frameIndex + 1) % SPINNER_CHARS.length;
      char.value = SPINNER_CHARS[frameIndex];
    }
  });

  return char;
}
