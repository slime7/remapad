/**
 * 手柄设置页：身份信息行与配色选择。
 * 设备对外只呈现 Pro Controller 2（单地址、单连接）：信息行显示序列号与对外
 * 广播地址；一行四个配色按钮，点一下就把该款式的四段颜色（机身 / 按键 /
 * 高光 / 握把，对应固件出厂块 0x13019 起的布局）整体写给固件——固件落盘、
 * 重建出厂块并打开连接窗口，主机自己连回来读到的就是新颜色（配色是主机连上
 * 时读的，所以换配色必须重新连一次）。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setControllerConfig } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PAD_H } from '../components/BottomPlaceholder';

/** 滚动列顶部内边距与块间距，与内容高度公式共用（pt-[34] + gap-2）。 */
const TOP_PAD = 34;
const GAP = 8;
/** 信息行行高（text-xs 单行）。 */
const ROW_H = 15;
/** 信息行卡高度：py-2 上下 16 + 行高 15 × 2 行 + 行距 8。 */
const INFO_CARD_H = 16 + ROW_H * 2 + GAP;
/** 配色行高度：48 尺寸圆钮一行，无外框。 */
const SWATCH_ROW_H = 48;

/**
 * 内容高度：信息卡与配色行两块。框架不回读布局，max 由 scroller 每帧读取。
 */
const contentHeight = () => TOP_PAD + INFO_CARD_H + SWATCH_ROW_H + GAP * 2;

/** 信息行槽位：0 = 序列号、1 = 对外广播地址。 */
type InfoSlot = 0 | 1;

/** 序列号占位值与固件 ns2_serial 产出同规则：前缀 + 10 位数字 + 校验位。 */
const PRO_SERIAL = 'HEJ71001123456';

/** 地址就绪前的占位（固件在 host 同步前给空串）。 */
const ADDRESS_PLACEHOLDER = '--';

/** 信息行文本样式：text-xs 单行常驻节点。 */
const INFO_ROW_CLASS = 'text-xs shrink-0';

/** 固件未配置时的四段配色（与 ns2 出厂块占位一致，见 firmware 的 factory_init）。 */
const DEFAULT_COLORS = { body: 0x232323, button: 0xa0a0a0, accent: 0xe6e6e6, grip: 0x323232 };

/**
 * 配色款式：一组四段颜色。取值参照已发售的 Pro Controller 2——标准黑（随主机
 * 发售，也是出厂默认）、Resident Evil Requiem 限定版的枪灰黑；银灰取标准款浅灰
 * 上盖 / 肩键的搭配；墨绿金来自已公布未发售的塞尔达 40 周年限定，做参考。
 */
interface Colorway {
  id: string;
  body: number;
  button: number;
  accent: number;
  grip: number;
}

const COLORWAYS: Colorway[] = [
  { id: 'standard', body: 0x232323, button: 0xa0a0a0, accent: 0xe6e6e6, grip: 0x323232 },
  { id: 'gunmetal', body: 0x3a4045, button: 0x9aa3ab, accent: 0xc8cdd2, grip: 0x2b2f33 },
  { id: 'silver', body: 0xb9bec4, button: 0x6e757c, accent: 0xe6e6e6, grip: 0x8a9096 },
  { id: 'zelda', body: 0x1e3b2a, button: 0xc8a24a, accent: 0xc8a24a, grip: 0x16301f },
];

/** 0xRRGGBB → `#rrggbb`（框架的样式值只接受 CSS 颜色串）。 */
function cssColor(rgb: number): string {
  return `#${rgb.toString(16).padStart(6, '0')}`;
}

/** 机身色的相对亮度（0-1）：决定选中环压在机身上时取浅色还是深色。 */
function luminance(rgb: number): number {
  const r = ((rgb >> 16) & 0xff) / 255;
  const g = ((rgb >> 8) & 0xff) / 255;
  const b = (rgb & 0xff) / 255;
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** 固件用 0 表示「未配置」：与款式比对时按出厂默认折算，页面上始终有一项选中。 */
function effective(value: number, fallback: number): number {
  return value === 0 ? fallback : value;
}

export function ControllerSettingsPage(props: {
  active: () => boolean;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  const config = () => hw.controllerConfig;
  /**
   * 信息行：序列号与对外广播地址各一行。两个文本节点常驻，值随固件应答更新
   * （地址在 host 同步前为空串）。槽位写死为两个节点而不写成列表：列表只要在
   * 构造时读到响应式状态，状态一变整段就会重挂。
   */
  const infoLabel = (slot: InfoSlot): string => (slot === 0 ? '序列号' : 'MAC');
  const infoValue = (slot: InfoSlot): string => {
    if (slot === 0) {
      return PRO_SERIAL;
    }
    return hw.controllerAddresses.pro || ADDRESS_PLACEHOLDER;
  };
  const infoText = (slot: InfoSlot): string => `${infoLabel(slot)} ${infoValue(slot)}`;

  /** 当前配色与某款式一致即为选中项（四段全比）。 */
  const isSelected = (colorway: Colorway): boolean => {
    const cfg = config();
    return (
      effective(cfg.bodyColor, DEFAULT_COLORS.body) === colorway.body &&
      effective(cfg.buttonColor, DEFAULT_COLORS.button) === colorway.button &&
      effective(cfg.accentColor, DEFAULT_COLORS.accent) === colorway.accent &&
      effective(cfg.gripColor, DEFAULT_COLORS.grip) === colorway.grip
    );
  };

  /** 点配色按钮：整组四段颜色下发给固件（持久化 + 重建出厂块）。 */
  const applyColorway = (colorway: Colorway): void => {
    setControllerConfig({
      bodyColor: colorway.body,
      buttonColor: colorway.button,
      accentColor: colorway.accent,
      gripColor: colorway.grip,
    });
  };

  /**
   * 选中环颜色：未选中时与机身同色（等于不画环），选中时取和机身对比的一侧
   * ——深机身用浅色环，浅机身（银灰这类）用深色环，否则浅底上的浅环看不见。
   */
  const ringColor = (colorway: Colorway): string => {
    if (!isSelected(colorway)) {
      return cssColor(colorway.body);
    }
    return luminance(colorway.body) > 0.5 ? COLOR.background : COLOR.onSurface;
  };

  const contentRef = usePageScroll(
    props.active,
    true,
    () => contentHeight() + BOTTOM_PAD_H,
    () => [],
  );

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View nodeRef={contentRef} class="w-full flex-col px-4 pt-[34] gap-2">
        <View class={STYLE.infoCardRows}>
          <Text class={INFO_ROW_CLASS} style={{ textColor: COLOR.onSurface }}>
            {infoText(0)}
          </Text>
          <Text class={INFO_ROW_CLASS} style={{ textColor: COLOR.onSurface }}>
            {infoText(1)}
          </Text>
        </View>

        {/* 配色按钮：一行四个 48 尺寸圆钮，等距铺满列宽（外框已去掉，给圆钮留宽度）。 */}
        <View class="w-full flex-row items-center justify-between shrink-0">
          {COLORWAYS.map((colorway) => (
            <View
              key={colorway.id}
              focusable={props.interactive()}
              onPress={() => applyColorway(colorway)}
              class={STYLE.colorSwatch}
              style={{ bgColor: cssColor(colorway.body) }}
            >
              <View
                class={STYLE.colorSwatchRing}
                style={{ borderColor: ringColor(colorway) }}
              />
            </View>
          ))}
        </View>

        <BottomPlaceholder />
      </View>
    </View>
  );
}
