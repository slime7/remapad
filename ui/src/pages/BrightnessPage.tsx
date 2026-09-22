/**
 * 亮度调节页：
 * 页根铺满整张 256 卡片，± 按钮是与背景瓣外弧同心的 64 圆钮（右上加、右下减），
 * 亮度档位以大号字居中显示，滑槽落在左侧瓣心列（左列无按钮，正好让它）。
 * 遵循 MD3 container / on-container 规范：
 * - 四叶草底色为 primaryContainer (#a6c8ff)；
 * - 表面直显文字（滑槽、档位）采用 onPrimaryContainer (#1b416f)；
 * - 角钮采用 secondaryContainer (#152a1f) 配 onSecondaryContainer (#92a99a)；
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

  // 垂直填充高度数值 (总槽高 120，上下内边距留白 4，总可填充高 112)
  const fillHeight = () => Math.round((Math.max(16, Math.min(100, currentLevel())) / 100) * 112);

  return (
    <View class={props.active() ? 'relative w-full h-full' : 'hidden'}>
      {/* 垂直滑槽（左列，定位见 STYLE.vSliderTrack） */}
      <View class={STYLE.vSliderTrack}>
        <View class={STYLE.vSliderFill} style={{ height: fillHeight() }} />
      </View>

      {/* 亮度档位：卡片正中、大号字（直显文本使用 onPrimaryContainer） */}
      <View class="absolute left-0 top-0 w-full h-full flex-row items-center justify-center">
        <Text class="text-2xl font-bold" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`${currentStep()}`}
        </Text>
      </View>

      {/* 加号按钮（右上瓣心，增加背光） */}
      <View
        focusable={props.interactive()}
        onPress={onStepUp}
        class={STYLE.cornerBtnTR}
      >
        <Text class="text-2xl font-bold" style={{ textColor: COLOR.onSecondaryContainer }}>
          +
        </Text>
      </View>

      {/* 减号按钮（右下瓣心，降低背光） */}
      <View
        focusable={props.interactive()}
        onPress={onStepDown}
        class={STYLE.cornerBtnBR}
      >
        <Text class="text-2xl font-bold" style={{ textColor: COLOR.onSecondaryContainer }}>
          −
        </Text>
      </View>
    </View>
  );
}
