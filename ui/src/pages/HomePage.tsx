/**
 * 状态页（首页）：两个圆形连接状态（USB 链路 / NS2 蓝牙），其余留白。
 * 圆形可点击：左进模式选择，右进手柄配对。
 * 其余页面由 App 在首帧之后逐帧补挂（见 App.tsx）。切页由本页根节点翻转
 * hidden 完成。
 * Vue Vapor：状态推导以函数形式在 JSX 内调用才会被渲染作用跟踪。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { hw } from '../hooks/useHardware';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { usbLinkState } from '../utils';
import type { TabKey } from '../components/AppNavBar';

function StateCircle(props: { glyph: string; iconColor: string; onPress: () => void }) {
  return (
    <View focusable onPress={props.onPress} class={STYLE.circle}>
      <Icon glyph={props.glyph} class="shrink-0 text-2xl" color={props.iconColor} />
    </View>
  );
}

export function HomePage(props: {
  active: () => boolean;
  onGo: (tab: TabKey) => void;
}) {
  const usbState = () => usbLinkState(hw.usbRole, hw.usbRoleActive);
  const usbGlyph = () =>
    usbState() === 'gamepad' ? ICON.gamepad : usbState() === 'computer' ? ICON.computer : usbState() === 'adb' ? ICON.adb : ICON.usbOff;
  const usbColor = () =>
    usbState() === 'gamepad' ? COLOR.tertiary : usbState() === 'computer' ? COLOR.primary : usbState() === 'adb' ? COLOR.secondary : COLOR.outline;
  const btConnected = () => hw.pairing === 'connected';

  // 首页内容固定一屏，不参与滚动。
  const scroller = usePageScroll(props.active, false);
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View
        class="w-full flex-col items-center px-4 pt-[38]"
        style={{ translateY: -scroller.offset() }}
      >
        <View class="flex-row gap-2 shrink-0">
          <StateCircle glyph={usbGlyph()} iconColor={usbColor()} onPress={() => props.onGo('mode')} />
          <StateCircle
            glyph={btConnected() ? ICON.bluetoothConnected : ICON.bluetoothDisabled}
            iconColor={btConnected() ? COLOR.primary : COLOR.outline}
            onPress={() => props.onGo('pairing')}
          />
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
