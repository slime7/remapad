/**
 * 手柄设置页（第 2 页）：
 * 位于四叶草中心区域 (130 × 130)。
 * 布局：
 * - 上层：手柄身份信息（序列号与对外伪装 MAC 地址）；
 * - 下层：2×2 紧凑排布的 4 种预设手柄款式色块（标准黑、枪灰黑、银灰、墨绿金）。
 * 内容固定不溢出不滚动，支持手柄上下移动焦点与确认选择。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';
import { hw, setControllerConfig } from '../hooks/useHardware';

const PRO_SERIAL = 'HEJ71001123456';
const ADDRESS_PLACEHOLDER = '--';
const DEFAULT_COLORS = { body: 0x232323, button: 0xa0a0a0, accent: 0xe6e6e6, grip: 0x323232 };

interface Colorway {
  id: string;
  name: string;
  body: number;
  button: number;
  accent: number;
  grip: number;
}

const COLORWAYS: Colorway[] = [
  { id: 'standard', name: '标准黑', body: 0x232323, button: 0xa0a0a0, accent: 0xe6e6e6, grip: 0x323232 },
  { id: 'gunmetal', name: '枪灰黑', body: 0x3a4045, button: 0x9aa3ab, accent: 0xc8cdd2, grip: 0x2b2f33 },
  { id: 'silver', name: '银灰', body: 0xb9bec4, button: 0x6e757c, accent: 0xe6e6e6, grip: 0x8a9096 },
  { id: 'zelda', name: '墨绿金', body: 0x1e3b2a, button: 0xc8a24a, accent: 0xc8a24a, grip: 0x16301f },
];

function cssColor(rgb: number): string {
  return `#${rgb.toString(16).padStart(6, '0')}`;
}

function luminance(rgb: number): number {
  const r = ((rgb >> 16) & 0xff) / 255;
  const g = ((rgb >> 8) & 0xff) / 255;
  const b = (rgb & 0xff) / 255;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

function effective(value: number, fallback: number): number {
  return value === 0 ? fallback : value;
}

export function ControllerSettingsPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const address = () => {
    const raw = hw.controllerAddresses.pro;
    return raw.length > 0 ? raw : ADDRESS_PLACEHOLDER;
  };

  const isSelected = (cw: Colorway) => {
    const cur = hw.controllerConfig;
    return (
      effective(cur.bodyColor, DEFAULT_COLORS.body) === cw.body &&
      effective(cur.buttonColor, DEFAULT_COLORS.button) === cw.button &&
      effective(cur.accentColor, DEFAULT_COLORS.accent) === cw.accent &&
      effective(cur.gripColor, DEFAULT_COLORS.grip) === cw.grip
    );
  };

  const selectColorway = (cw: Colorway) => {
    setControllerConfig({
      bodyColor: cw.body,
      buttonColor: cw.button,
      accentColor: cw.accent,
      gripColor: cw.grip,
    });
  };

  const ringBorder = (cw: Colorway) => {
    if (!isSelected(cw)) {
      return cssColor(cw.body);
    }
    return luminance(cw.body) > 0.4 ? '#000000' : '#ffffff';
  };

  /** 单个配色按钮：42 圆形色块 + 选中指示环。
   *  环的底色与色块同色——引擎只把「有底色的圆角边框」画成两枚圆角填充
   *  （各 4 个圆角四边形 + 3 条矩形），纯边框元素退化成一整串逐行覆盖条，
   *  36 尺寸的环就是两百来条矩形指令（见 docs/adr/0050）。 */
  const swatch = (cw: Colorway) => (
    <View
      key={cw.id}
      focusable={props.interactive()}
      onPress={() => selectColorway(cw)}
      class={STYLE.colorSwatch42}
      style={{ bgColor: cssColor(cw.body) }}
    >
      <View
        class={STYLE.colorSwatchRing42}
        style={{ borderColor: ringBorder(cw), bgColor: cssColor(cw.body) }}
      />
    </View>
  );

  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-1 gap-[6]' : 'hidden'}>
      {/* 上层：手柄身份标识（去掉了标题手柄名，留出高度给 42 尺寸大圆钮） */}
      <View class="flex-col items-center gap-[1] shrink-0">
        <Text class="text-xs shrink-0 font-bold" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`SN: ${PRO_SERIAL}`}
        </Text>
        <Text class="text-xs shrink-0 font-bold" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`MAC: ${address()}`}
        </Text>
      </View>

      {/* 下层：4 款配色选择，2×2 饱满排布，达到 42 触发尺寸 */}
      <View class="flex-col items-center gap-[6] shrink-0 mt-[2]">
        <View class="flex-row items-center gap-4">{COLORWAYS.slice(0, 2).map(swatch)}</View>
        <View class="flex-row items-center gap-4">{COLORWAYS.slice(2, 4).map(swatch)}</View>
      </View>
    </View>
  );
}
