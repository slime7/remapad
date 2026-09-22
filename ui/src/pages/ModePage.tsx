/**
 * USB 模式页：
 * 板卡只有一根 Type-C，端口在两种角色里二选一——「串口」把它交给 PC
 * （桥接帧、烧录与日志），「手柄」交给 OTG host 直插手柄
 * （见 docs/adr/0027 与 docs/hardware.md 的「USB 控制器复用」）。
 * 切到「手柄」后 PC 上的串口消失，只能在本屏幕切回或重启设备，因此切过去
 * 之前由 App 弹确认；切回「串口」后询问是否立刻重启（复位是保底恢复路径）。
 * 配色只用 theme.ts 的 MD3 语义 token：选中卡是 onPrimaryContainer 深蓝底配
 * primaryContainer 浅蓝字，未选中卡是比面色更亮的浅底配 onPrimaryContainer 字。
 * 两张卡的图标与底栏左槽的模式图标一一对应（串口 = 台式电脑、手柄 = 手柄）。
 * 布局：标题 + 两张选项卡，都在四叶草中央内容框内，页面不滚动，内容不出框。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { hw, setUsbRole } from '../hooks/useHardware';
import { COLOR, STYLE } from '../theme';

/** 一张选项卡：图标 + 标题 + 一句说明（文案只写这一档能做什么）。 */
function ModeCard(props: {
  title: string;
  desc: string;
  glyph: string;
  selected: boolean;
  /** 页面在画面上时才参与焦点遍历（见 App.tsx 的 interactive）。 */
  interactive: () => boolean;
  onSelect: () => void;
}) {
  return (
    <View
      focusable={props.interactive()}
      onPress={props.onSelect}
      class={props.selected ? STYLE.optionCardSel : STYLE.optionCard}
    >
      <Icon
        glyph={props.glyph}
        class="shrink-0 text-2xl"
        color={props.selected ? COLOR.primaryContainer : COLOR.onPrimaryFixedVariant}
      />
      <View class="flex-col grow">
        <Text
          class="text-sm font-bold"
          style={{ textColor: props.selected ? COLOR.primaryContainer : COLOR.onPrimaryContainer }}
        >
          {props.title}
        </Text>
        <Text
          class="text-xs"
          style={{
            textColor: props.selected ? COLOR.primaryContainer : COLOR.onPrimaryContainer,
          }}
        >
          {props.desc}
        </Text>
      </View>
    </View>
  );
}

export function ModePage(props: {
  active: () => boolean;
  /** 切到「手柄」要先弹确认（PC 串口会消失），弹窗由 App 统一持有。 */
  onAskHost: () => void;
  interactive: () => boolean;
}) {
  return (
    <View
      class={
        props.active()
          ? 'w-full h-full flex-col items-center justify-center gap-2 px-1'
          : 'hidden'
      }
    >
      <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
        USB 模式
      </Text>
      <ModeCard
        title="串口"
        desc="PC 桥接与烧录"
        glyph={ICON.desktopWindows}
        selected={hw.usbRole === 'device'}
        interactive={props.interactive}
        onSelect={() => {
          /* 切回串口是恢复方向：直接切，切完由 App 问要不要立刻重启。 */
          if (hw.usbRole !== 'device') {
            setUsbRole('device');
          }
        }}
      />
      <ModeCard
        title="手柄"
        desc="直插手柄输入"
        glyph={ICON.videogameAsset}
        selected={hw.usbRole === 'host'}
        interactive={props.interactive}
        onSelect={() => {
          if (hw.usbRole !== 'host') {
            props.onAskHost();
          }
        }}
      />
    </View>
  );
}
