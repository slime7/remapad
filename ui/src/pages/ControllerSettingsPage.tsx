/**
 * 手柄设置页：手柄类型（Pro 默认 / JoyCon 组合）、身份信息行与机身颜色
 * 预留。类型选择经 bridge 持久化到固件 NVS 并应用于 NS2 出厂块（序列号 /
 * PID / 配色）；颜色选择 UI 预留，色块暂不可点，字段随配置先持久化。
 * JoyCon 组合是左右两只各自独立连接（主机的 Grip/顺序界面只用于排序与
 * 确认），序列号按 HBW10067 / HCW10068 前缀各显一条，MAC 取固件给出的
 * 对外广播地址（Pro 公共伪装地址，JoyCon 各自派生）。
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
/** 类型卡高度：py-3 上下 24 + 图标行 29（text-2xl 槽位 cell 高）。 */
const CARD_H = 24 + 29;
/** 信息行行高（text-xs 单行）。 */
const ROW_H = 15;
/** 信息行卡高度：py-2 上下 16 + 行高 15 × 行数 + 行距 8 ×（行数 − 1）。 */
const INFO_CARD_H_PRO = 16 + ROW_H * 2 + GAP;
const INFO_CARD_H_JOYCON = 16 + ROW_H * 4 + GAP * 3;
/** 颜色卡高度：py-3 上下 24 + 标题行 18 + mt-2 8 + 色块 24。 */
const COLOR_CARD_H = 24 + 18 + 8 + 24;

/**
 * 内容高度按当前形态算：JoyCon 的信息行卡多两行，其余与 Pro 相同。
 * 框架不回读布局，max 由 scroller 每帧读取，切换形态后滚动范围随之更新。
 */
const contentHeight = (joycon: boolean) =>
  TOP_PAD +
  CARD_H * 2 +
  (joycon ? INFO_CARD_H_JOYCON : INFO_CARD_H_PRO) +
  COLOR_CARD_H +
  GAP * 4;

interface TypeOption {
  type: ControllerType;
  title: string;
}

/** 信息行槽位：0/1 = Pro 或 JoyCon 左（序列号 / MAC），2/3 = JoyCon 右。 */
type InfoSlot = 0 | 1 | 2 | 3;

/** 序列号占位值与固件 ns2_serial 产出同规则：前缀 + 10 位数字 + 校验位。 */
const PRO_SERIAL = 'HEJ71001123456';
const JOYCON_SERIAL_LEFT = 'HBW10067012342';
const JOYCON_SERIAL_RIGHT = 'HCW10068012341';

/** 地址就绪前的占位（固件在 host 同步前给空串）。 */
const ADDRESS_PLACEHOLDER = '--';

/** 信息行文本样式：text-xs 单行常驻节点。 */
const INFO_ROW_CLASS = 'text-xs shrink-0';

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
  /** 页面在画面上时才参与焦点遍历（见 App.tsx 的 interactive）。 */
  interactive: () => boolean;
}) {
  return (
    <View
      focusable={props.interactive()}
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

export function ControllerSettingsPage(props: {
  active: () => boolean;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  const config = () => hw.controllerConfig;
  const isJoycon = () => config().type === 'joycon';
  /**
   * 信息行：Pro 两行（序列号、MAC），JoyCon 组合四行（左序列号、左 MAC、
   * 右序列号、右 MAC）。四个文本节点常驻，切换形态只重写文本与 hidden
   * （hidden 是 display:none，右只两行收起后不占行高）。槽位写死为四个
   * 节点而不写成列表：列表只要在构造时读到响应式状态，状态一变整段就会重挂。
   */
  const infoLabel = (slot: InfoSlot): string => {
    if (slot === 2) {
      return '右序列号';
    }
    if (slot === 3) {
      return '右 MAC';
    }
    if (isJoycon()) {
      return slot === 0 ? '左序列号' : '左 MAC';
    }
    return slot === 0 ? '序列号' : 'MAC';
  };
  /**
   * 取值按当前形态算。序列号规则：前缀 + 10 位数字 + 校验位（S = 偶位和 +
   * 3×奇位和，校验位 = (10 − S mod 10) mod 10），与固件 ns2_serial 一致；
   * 地址来自固件的 controllerConfig 应答。
   */
  const infoValue = (slot: InfoSlot): string => {
    const addresses = hw.controllerAddresses;
    if (slot === 2) {
      return JOYCON_SERIAL_RIGHT;
    }
    if (slot === 3) {
      return addresses.right || ADDRESS_PLACEHOLDER;
    }
    if (isJoycon()) {
      return slot === 0 ? JOYCON_SERIAL_LEFT : addresses.left || ADDRESS_PLACEHOLDER;
    }
    return slot === 0 ? PRO_SERIAL : addresses.pro || ADDRESS_PLACEHOLDER;
  };
  /** 右只两行只在 JoyCon 组合下出现；hidden 是 display:none，不占行距。 */
  const infoRowClass = (slot: InfoSlot): string =>
    slot >= 2 && !isJoycon() ? 'hidden' : INFO_ROW_CLASS;
  const infoText = (slot: InfoSlot): string => `${infoLabel(slot)} ${infoValue(slot)}`;

  const contentRef = usePageScroll(props.active, true, () => contentHeight(isJoycon()) + BOTTOM_PAD_H);

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View nodeRef={contentRef} class="w-full flex-col px-4 pt-[34] gap-2">
        {TYPE_OPTIONS.map((option) => (
          <TypeCard
            option={option}
            selected={config().type === option.type}
            onSelect={() => setControllerConfig({ ...config(), type: option.type })}
            interactive={props.interactive}
          />
        ))}

        <View class={STYLE.infoCardRows}>
          <Text class={infoRowClass(0)} style={{ textColor: COLOR.onSurface }}>
            {infoText(0)}
          </Text>
          <Text class={infoRowClass(1)} style={{ textColor: COLOR.onSurface }}>
            {infoText(1)}
          </Text>
          <Text class={infoRowClass(2)} style={{ textColor: COLOR.onSurface }}>
            {infoText(2)}
          </Text>
          <Text class={infoRowClass(3)} style={{ textColor: COLOR.onSurface }}>
            {infoText(3)}
          </Text>
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
