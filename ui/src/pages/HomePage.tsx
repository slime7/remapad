/**
 * 状态页（首页）：两个圆形连接状态（USB 链路 / NS2 蓝牙）、其下的玩家序号
 * 四格指示灯，其余留白。
 * 圆形可点击：左进模式选择，右进手柄配对。
 * 其余页面由 App 在首帧之后逐帧补挂（见 App.tsx）。切页由本页根节点翻转
 * hidden 完成。
 * Vue Vapor：状态推导以函数形式在 JSX 内调用才会被渲染作用跟踪。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { hw } from '../hooks/useHardware';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { usbLinkState } from '../utils';
import type { TabKey } from '../components/AppNavBar';

function StateCircle(props: {
  glyph: string;
  iconColor: string;
  onPress: () => void;
  /** 页面在画面上时才参与焦点遍历（见 App.tsx 的 interactive）。 */
  interactive: () => boolean;
}) {
  return (
    <View focusable={props.interactive()} onPress={props.onPress} class={STYLE.circle}>
      <Icon glyph={props.glyph} class="shrink-0 text-2xl" color={props.iconColor} />
    </View>
  );
}

/**
 * 玩家序号四格指示灯：NS2 主机在注册手柄后用 Command 0x09 下发 4 位掩码，
 * bit0-3 依次对应从左到右四格（与手柄上的序号灯一致）；没有主机下发时掩码
 * 为 0，四格全灭。方块的 class 在子组件里按 props 取值，掩码变化只重画行内
 * 四格，不重建首页其它节点。间距由本行自己给出：它是首页专属的一段留白，
 * 与上方两枚状态圆不是同一组控件。
 */
function PlayerLedRow(props: { mask: number }) {
  return (
    <View class="flex-row gap-2 shrink-0 mt-[18]">
      <View class={(props.mask & 0b0001) !== 0 ? STYLE.playerLedOn : STYLE.playerLedOff} />
      <View class={(props.mask & 0b0010) !== 0 ? STYLE.playerLedOn : STYLE.playerLedOff} />
      <View class={(props.mask & 0b0100) !== 0 ? STYLE.playerLedOn : STYLE.playerLedOff} />
      <View class={(props.mask & 0b1000) !== 0 ? STYLE.playerLedOn : STYLE.playerLedOff} />
    </View>
  );
}

export function HomePage(props: {
  active: () => boolean;
  onGo: (tab: TabKey) => void;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  const usbState = () => usbLinkState(hw.usbRole, hw.usbRoleActive);
  const usbGlyph = () =>
    usbState() === 'gamepad' ? ICON.gamepad : usbState() === 'computer' ? ICON.computer : usbState() === 'adb' ? ICON.adb : ICON.usbOff;
  const usbColor = () =>
    usbState() === 'gamepad' ? COLOR.tertiary : usbState() === 'computer' ? COLOR.primary : usbState() === 'adb' ? COLOR.secondary : COLOR.outline;
  const btConnected = () => hw.pairing === 'connected';

  // 首页内容固定一屏，不参与滚动，也就不需要注册滚动。
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View
        class="w-full flex-col items-center px-4 pt-[38]"
      >
        <View class="flex-row gap-2 shrink-0">
          <StateCircle
            glyph={usbGlyph()}
            iconColor={usbColor()}
            onPress={() => props.onGo('mode')}
            interactive={props.interactive}
          />
          <StateCircle
            glyph={btConnected() ? ICON.bluetoothConnected : ICON.bluetoothDisabled}
            iconColor={btConnected() ? COLOR.primary : COLOR.outline}
            onPress={() => props.onGo('pairing')}
            interactive={props.interactive}
          />
        </View>
        <PlayerLedRow mask={hw.playerLed} />
        <BottomPlaceholder />
      </View>
    </View>
  );
}
