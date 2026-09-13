/**
 * 状态页（首页）：两个圆形连接状态（USB 链路 / NS2 蓝牙），其余留白。
 * 圆形可点击：左进模式选择，右进手柄配对。
 * 其余页面首次进入时才挂载并逐帧填充（见 App.tsx 与
 * docs/adr/0014-page-mount-on-demand-progressive-fill.md），填充期间在本页留白
 * 处显示加载提示。切页由本页根节点翻转 hidden 完成。
 * Vue Vapor：状态推导以函数形式在 JSX 内调用才会被渲染作用跟踪。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { hw } from '../hooks/useHardware';
import { COLOR, STYLE } from '../theme';
import { BOTTOM_PAD_H } from '../components/BottomPlaceholder';
import { SPINNER_FRAMES } from '../spinner';
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
  /** 其余页面尚未挂载完成 —— 在圆钮下方的留白处显示加载提示。 */
  loading: () => boolean;
}) {
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: 3 });
  const usbState = () => usbLinkState(hw.usbRole, hw.usbRoleActive);
  const usbGlyph = () =>
    usbState() === 'gamepad' ? ICON.gamepad : usbState() === 'computer' ? ICON.computer : usbState() === 'adb' ? ICON.adb : ICON.usbOff;
  const usbColor = () =>
    usbState() === 'gamepad' ? COLOR.tertiary : usbState() === 'computer' ? COLOR.primary : usbState() === 'adb' ? COLOR.secondary : COLOR.outline;
  const btConnected = () => hw.pairing === 'connected';

  const scroller = usePageScroll(props.active, () => 38 + 56 + BOTTOM_PAD_H);
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View
        class={STYLE.scrollColumnCenter}
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
        <View class="grow w-full flex-col items-center justify-center">
          {/* 常驻节点，按 class 收起：加载提示会出现/消失，不能用条件渲染反复卸载重建。 */}
          <View class={props.loading() ? 'flex-col items-center gap-2' : 'hidden'}>
            {/* 收起时不读动画帧：否则常驻的 Image 会每 3 帧换一次图（实测每帧约 +9 ms）。 */}
            <Image
              class="w-[24] h-[24] shrink-0"
              src={props.loading() ? spinnerSrc.value : SPINNER_FRAMES[0]}
            />
            <Text class="text-xs" style={{ textColor: COLOR.onSurfaceVariant }}>
              加载中…
            </Text>
          </View>
        </View>
      </View>
    </View>
  );
}
