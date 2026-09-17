/**
 * 亮度调节页（第 1 页）：
 * 位于四叶草中心区域 (120 × 120)。
 * 遵循 MD3 container / on-container 规范：
 * - 四叶草底色为 primaryContainer (#a6c8ff)；
 * - 表面直显文字百分比采用 onPrimaryContainer (#1b416f)；
 * - 嵌套加减号按钮采用 surfaceContainer (#0d1a2c) 配 onSurface (#d9e6ff)；
 * - 加减号使用无损排版字符 '+' 与 '−'，杜绝图标字体缺失导致的豆腐块。
 */
import { View, Text } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';
import { hw, setBacklight } from '../hooks/useHardware';

const BACKLIGHT_STEPS = [20, 40, 60, 80, 100] as const;

export function BrightnessPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const currentLevel = () => hw.backlight;

  // 亮度 5 档：20% 对应 1 档，40% 对应 2 档 ... 100% 对应 5 档
  const currentStep = () => Math.max(1, Math.min(5, Math.round(hw.backlight / 20)));

  const onStepUp = () => {
    const cur = currentLevel();
    const next = BACKLIGHT_STEPS.find((val) => val > cur) ?? 100;
    setBacklight(next);
  };

  const onStepDown = () => {
    const cur = currentLevel();
    const rev = [...BACKLIGHT_STEPS].reverse();
    const next = rev.find((val) => val < cur) ?? 20;
    setBacklight(next);
  };

  // 垂直填充高度数值 (总槽高 96，上下内边距留白 4，总可填充高 88)
  const fillHeight = () => Math.round((Math.max(16, Math.min(100, currentLevel())) / 100) * 88);

  return (
    <View class={props.active() ? 'w-full h-full flex-row items-center justify-center gap-4' : 'hidden'}>
      {/* 左侧垂直滑槽 (bg: onPrimaryContainer 深蓝, fill: 白色高亮) */}
      <View class={STYLE.vSliderTrack}>
        <View class={STYLE.vSliderFill} style={{ height: fillHeight() }} />
      </View>

      {/* 右侧上下加减按钮与读数 */}
      <View class="flex-col items-center justify-between h-[96] py-[2]">
        {/* 加号按钮（增加背光） */}
        <View
          focusable={props.interactive()}
          onPress={onStepUp}
          class={STYLE.brightnessBtn}
        >
          <Text class="text-lg font-bold" style={{ textColor: COLOR.onSurface }}>
            +
          </Text>
        </View>

        {/* 亮度档位数值 1-5 (直显文本使用 onPrimaryContainer) */}
        <Text class="text-sm shrink-0 font-bold" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`${currentStep()}`}
        </Text>

        {/* 减号按钮（降低背光） */}
        <View
          focusable={props.interactive()}
          onPress={onStepDown}
          class={STYLE.brightnessBtn}
        >
          <Text class="text-lg font-bold" style={{ textColor: COLOR.onSurface }}>
            −
          </Text>
        </View>
      </View>
    </View>
  );
}
