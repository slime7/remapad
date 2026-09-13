/**
 * 手柄设置页：手柄类型（Pro 默认 / JoyCon 组合）、序列号信息与机身颜色
 * 预留。类型选择经 bridge 持久化到固件 NVS 并应用于 NS2 出厂块（序列号 /
 * PID / 配色）；颜色选择 UI 预留，色块暂不可点，字段随配置先持久化。
 * JoyCon 组合是左右两只各自独立连接（主机的 Grip/顺序界面只用于排序与
 * 确认），序列号按 HBW10067 / HCW10068 前缀各显一条。类型说明行可能超出
 * 卡片宽度，交给 MarqueeText 滚动展示。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setControllerConfig } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
import { MarqueeText } from '../components/MarqueeText';
import type { ControllerType } from '../bridge/protocol';

/** 选项卡高度：py-3 上下 24 + 标题行 18 + 描述行 15。 */
const CARD_H = 24 + 18 + 15;
/** 序列号卡高度：py-2 上下 16 + 标题行 18 + 两行信息 22×2。 */
const SERIAL_CARD_H = 16 + 18 + 22 * 2;
/** 颜色卡高度：py-3 上下 24 + 标题行 18。 */
const COLOR_CARD_H = 24 + 18;
/** 类型卡说明行可视宽度：卡片 208 − px-3 左右 24 − 图标 24 − gap-3 12。 */
const TYPE_DESC_W = 148;

interface TypeOption {
  type: ControllerType;
  title: string;
  desc: string;
}

const TYPE_OPTIONS: TypeOption[] = [
  { type: 'pro', title: 'Pro 手柄', desc: '单设备 · PID 0x2069 · 默认' },
  { type: 'joycon', title: 'JoyCon 组合', desc: '左右两只各自独立连接 · 主机 Grip 界面确认' },
];

/** 预留色卡（机身配色候选，0xRRGGBB）；选择功能实装前不可点。 */
const COLOR_PRESETS: Array<{ label: string; rgb: number }> = [
  { label: '灰', rgb: 0x232323 },
  { label: '红', rgb: 0x8a1a1e },
  { label: '蓝', rgb: 0x154e77 },
  { label: '黄', rgb: 0xb8a038 },
];

function rgbLabel(rgb: number): string {
  const hex = rgb.toString(16).padStart(6, '0').toUpperCase();
  return `#${hex}`;
}

function TypeCard(props: {
  option: TypeOption;
  selected: boolean;
  onSelect: () => void;
  active: () => boolean;
}) {
  return (
    <View
      focusable
      onPress={props.onSelect}
      class={props.selected ? STYLE.optionCardSel : STYLE.optionCard}
    >
      <Icon
        glyph={ICON.gamepad}
        class="shrink-0 text-2xl"
        color={props.selected ? COLOR.onPrimaryContainer : COLOR.primary}
      />
      <View class="flex-col grow">
        <Text
          class="text-sm font-bold"
          style={{ textColor: props.selected ? COLOR.onPrimaryContainer : COLOR.onSurface }}
        >
          {props.option.title}
        </Text>
        <MarqueeText
          text={props.option.desc}
          width={TYPE_DESC_W}
          color={props.selected ? COLOR.onPrimary : COLOR.onSurfaceVariant}
          active={props.active}
        />
      </View>
    </View>
  );
}

function InfoRow(props: { label: string; value: string }) {
  return (
    <View class="w-full h-[22] shrink-0 flex-row items-center">
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {props.label}
      </Text>
      <View class="grow" />
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurface }}>
        {props.value}
      </Text>
    </View>
  );
}

export function ControllerSettingsPage(props: { active: () => boolean }) {
  const config = () => hw.controllerConfig;
  const isJoycon = () => config().type === 'joycon';
  // 序列号与固件 ns2_serial 生成的常量一致：前缀 + 10 位数字 + 校验位
  // （S = 偶位和 + 3×奇位和，校验位 = (10 − S mod 10) mod 10）。
  const serials = () =>
    isJoycon()
      ? [
          { label: '左序列号', value: 'HBW10067012342' },
          { label: '右序列号', value: 'HCW10068012341' },
        ]
      : [{ label: '序列号', value: 'HEJ71001123456' }];

  const scroller = usePageScroll(
    props.active,
    () => 34 + CARD_H * 2 + 8 * 2 + 15 + SERIAL_CARD_H + 8 + COLOR_CARD_H + 8 + BOTTOM_PLACEHOLDER_H,
  );

  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          手柄类型
        </Text>
        {TYPE_OPTIONS.map((option) => (
          <TypeCard
            option={option}
            selected={config().type === option.type}
            onSelect={() => setControllerConfig({ ...config(), type: option.type })}
            active={props.active}
          />
        ))}

        <View class={STYLE.infoCard}>
          <Text class="text-sm font-bold shrink-0" style={{ textColor: COLOR.onSurface }}>
            序列号信息
          </Text>
          {serials().map((row) => (
            <InfoRow label={row.label} value={row.value} />
          ))}
        </View>

        <View class={STYLE.actionCard}>
          <View class="w-full h-[18] flex-row items-center shrink-0">
            <Text class="text-sm font-bold grow" style={{ textColor: COLOR.onSurface }}>
              机身颜色
            </Text>
            <Text class="text-xs" style={{ textColor: COLOR.onSurfaceVariant }}>
              {rgbLabel(config().bodyColor)} · 即将支持
            </Text>
          </View>
          <View class="flex-row gap-2 mt-2 shrink-0">
            {COLOR_PRESETS.map((preset) => (
              <View
                key={preset.label}
                class="w-[24] h-[24] rounded-full shrink-0"
                style={{
                  bgColor: `#${preset.rgb.toString(16).padStart(6, '0')}`,
                  borderColor: preset.rgb === config().bodyColor ? COLOR.primary : COLOR.outlineVariant,
                  borderWidth: 1,
                }}
              />
            ))}
          </View>
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
