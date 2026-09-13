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

/** 序列号槽位：0 = Pro 单手柄 / JoyCon 左，1 = JoyCon 右（Pro 形态收起）。 */
type SerialSlot = 0 | 1;

/** 序列号占位值与固件 ns2_serial 产出同规则：前缀 + 10 位数字 + 校验位。 */
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

/**
 * 序列号行：两行都是常驻节点，切换形态只重写文本与 hidden。
 *
 * 三个字段必须以表达式传入（编译器会包成逐属性 getter，框架读 props 时解包并
 * 跟踪依赖）。若改成在组件里拼数组再 map —— 或让列表构造读到响应式状态 ——
 * 该状态一变整段列表就会重挂：实测一次切换要重建 13 个原生节点，实机按每节点
 * 约 50 ms 的建树成本计会卡半秒以上。
 */
function InfoRow(props: { label: string; value: string; hidden: boolean }) {
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
  // 序列号规则：前缀 + 10 位数字 + 校验位（S = 偶位和 + 3×奇位和，
  // 校验位 = (10 − S mod 10) mod 10），与固件 ns2_serial 一致。
  const serialLabel = (slot: SerialSlot): string => {
    if (slot === 1) {
      return '右序列号';
    }
    return isJoycon() ? '左序列号' : '序列号';
  };
  /**
   * 序列号取值按当前形态算。真机上机身配色与序列号属于同一份身份，配色可选后
   * 这里要一并按 bodyColor 取值；若 NS2 不校验配色与序列号的对应关系，也可以
   * 复用同一份序列号。两种做法都只改这个函数，绑定方式不变。
   */
  const serialValue = (slot: SerialSlot): string => {
    if (slot === 1) {
      return JOYCON_SERIAL_RIGHT;
    }
    return isJoycon() ? JOYCON_SERIAL_LEFT : PRO_SERIAL;
  };
  /** Pro 形态只有一条序列号：槽位 1 收 hidden，节点常驻不重建。 */
  const serialHidden = (slot: SerialSlot): boolean => slot === 1 && !isJoycon();

  const contentRef = usePageScroll(props.active, true, () => contentHeight(isJoycon()) + BOTTOM_PAD_H);

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View nodeRef={contentRef} class="w-full flex-col px-4 pt-[34] gap-2">
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
          <InfoRow label={serialLabel(0)} value={serialValue(0)} hidden={serialHidden(0)} />
          <InfoRow label={serialLabel(1)} value={serialValue(1)} hidden={serialHidden(1)} />
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
