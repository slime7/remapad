import { ticksForStepAt60Hz } from './tick';

/** 载入指示图：8 帧 SVG 序列，构建期进 pak；配对页与首页加载提示共用。 */
export const SPINNER_FRAMES = [
  'spinner-00.svg',
  'spinner-01.svg',
  'spinner-02.svg',
  'spinner-03.svg',
  'spinner-04.svg',
  'spinner-05.svg',
  'spinner-06.svg',
  'spinner-07.svg',
] as const;

/** 每张序列图的步进基准（60 Hz 下的 frameStep）：8 帧一圈 0.4 秒。 */
const SPINNER_STEP_AT_60HZ = 3;

/** 当前 tick 下的整数帧步进（30 Hz 下 1.5 帧取整为 2 帧），给 createSpriteAnimation。 */
export const SPINNER_STEP = Math.max(
  1,
  Math.round(ticksForStepAt60Hz(SPINNER_STEP_AT_60HZ)),
);
