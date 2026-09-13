/**
 * 手柄设置页：手柄类型（Pro 默认 / JoyCon 组合）、序列号信息与机身颜色
 * 预留。类型选择经 bridge 持久化到固件 NVS 并应用于 NS2 出厂块（序列号 /
 * PID / 配色）；颜色选择 UI 预留，色块暂不可点，字段随配置先持久化。
 * JoyCon 组合是左右两只各自独立连接（主机的 Grip/顺序界面只用于排序与
 * 确认），序列号按 HBW10067 / HCW10068 前缀各显一条。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setControllerConfig } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PAD_H } from '../components/BottomPlaceholder';
import type { ControllerType } from '../bridge/protocol';

/** 滚动列顶部内边距与块间距，与内容高度公式共用（pt-[34] + gap-2）。 */
const TOP_PAD = 34;
const GAP = 8;
/** 类型标签行高度（text-xs 单行）。 */
const LABEL_H = 15;
/** 信息行高度。 */
const ROW_H = 22;
/** 类型卡高度：py-3 上下 24 + 图标行 29（text-2xl 槽位 cell 高）。 */
const CARD_H = 24 + 29;
/** 序列号卡高度：py-2 上下 16 + 标题行 18 + 序列号行 22 × 行数。 */
const INFO_CARD_H = 16 + 18 + ROW_H * 2;
/** 颜色卡高度：py-3 上下 24 + 标题行 18 + mt-2 8 + 色块 24。 */
const COLOR_CARD_H = 24 + 18 + 8 + 24;

/**
 * 内容高度按当前形态算：JoyCon 的序列号卡多一行，其余与 Pro 相同。
 * 框架不回读布局，max 由 scroller 每帧读取，切换形态后滚动范围随之更新。
 */
const contentHeight = (joycon: boolean) =>
  TOP_PAD +
  LABEL_H +
  CARD_H * 2 +
  (joycon ? INFO_CARD_H : INFO_CARD_H - ROW_H) +
  COLOR_CARD_H +
  GAP * 4;

interface TypeOption {
  type: ControllerType;
  title: string;
}

/** 序列号行：两行结构固定，非当前形态的那行用 hidden 收起，避免切换时重建节点。 */
interface SerialRow {
  label: string;
  value: string;
  hidden: boolean;
}

const PRO_SERIAL = 'HEJ71001123456';
const JOYCON_SERIAL_LEFT = 'HBW10067012342';
const JOYCON_SERIAL_RIGHT = 'HCW10068012341';

const TYPE_OPTIONS: TypeOption[] = [
  { type: 'pro', title: 'Pro 手柄' },
  { type: 'joycon', title: 'JoyCon 组合' },
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
      <Text
        class="text-sm font-bold"
        style={{ textColor: props.selected ? COLOR.onPrimaryContainer : COLOR.onSurface }}
      >
        {props.option.title}
      </Text>
    </View>
  );
}

function InfoRow(props: { label: string; value: string; hidden?: boolean }) {
  return (
    <View
      class={
        props.hidden
          ? 'hidden'
          : 'w-full h-[22] shrink-0 flex-row items-center justify-between'
      }
    >
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {props.label}
      </Text>
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
  const serialRows = (): SerialRow[] =>
    isJoycon()
      ? [
          { label: '左序列号', value: JOYCON_SERIAL_LEFT, hidden: false },
          { label: '右序列号', value: JOYCON_SERIAL_RIGHT, hidden: false },
        ]
      : [
          { label: '序列号', value: PRO_SERIAL, hidden: false },
          { label: '右序列号', value: JOYCON_SERIAL_RIGHT, hidden: true },
        ];

  const scroller = usePageScroll(props.active, true, () => contentHeight(isJoycon()) + BOTTOM_PAD_H);

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View class="w-full flex-col px-4 pt-[34] gap-2" style={{ translateY: -scroller.offset() }}>
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          手柄类型
        </Text>
        {TYPE_OPTIONS.map((option) => (
          <TypeCard
            option={option}
            selected={config().type === option.type}
            onSelect={() => setControllerConfig({ ...config(), type: option.type })}
          />
        ))}

        <View class={STYLE.infoCard}>
          <Text class="text-sm font-bold shrink-0" style={{ textColor: COLOR.onSurface }}>
            序列号信息
          </Text>
          {serialRows().map((row) => (
            <InfoRow label={row.label} value={row.value} hidden={row.hidden} />
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
                  borderColor:
                    preset.rgb === config().bodyColor ? COLOR.primary : COLOR.outlineVariant,
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
