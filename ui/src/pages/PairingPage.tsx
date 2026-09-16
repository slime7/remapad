/**
 * 配对页：连接键 + 配对新主机 + JoyCon 的 L+R 兜底。
 * 设备与真机一样不主动发信号：上电与主机睡下（断连）都静默，主按钮就是连接键
 * ——未连接时按它打开连接窗口（已配对发回连形态等主机连回来，未配对进配对
 * 流程发发现广播），广播中它变「停止」收掉一切回到静默，链路在线时变「断开」
 * 放下当前主机（想换玩家序号就先断开，主机在握把/顺序页时再按连接）。
 * 副按钮配新主机：断开当前主机后进发现广播等新主机搜（主机眼里是另一台
 * 设备），配对流程中它换成 L+R 兜底（JoyCon 手动确认，Pro 注入按键）。
 * 主机睡下时用调试页的「唤醒 HOME」把它叫起来。屏幕上的 PWR 长按等价于
 * 主按钮。内容单屏放得下，静态页不挂滚动。七个页面在首屏前一次挂完，切页由
 * 根节点翻转 hidden 完成（见 docs/adr/0016-mount-all-pages-before-first-frame.md）。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { connect, disconnect, hw, pressLr, startPairing } from '../hooks/useHardware';
import { SPINNER_FRAMES, SPINNER_STEP } from '../spinner';
import { pairingColor, pairingLabel } from '../utils';

export function PairingPage(props: {
  active: () => boolean;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  /* 步进由公共帧节奏算出（见 spinner.ts）。 */
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: SPINNER_STEP });
  // Vue Vapor：条件以函数形式在 JSX 内调用才会被渲染作用跟踪。
  /** 正在广播（配对流程或连接窗口）：按钮变「停止」，spinner 转起来。 */
  const broadcasting = () => hw.pairing === 'scanning' || hw.pairing === 'advertising';
  /** 链路在线（含注册握手未完成的主机）：按钮变「断开」。 */
  const linkUp = () => hw.pairing === 'pairing' || hw.pairing === 'connected';
  const busy = () => broadcasting() || linkUp();
  const mainLabel = () => (linkUp() ? '断开' : broadcasting() ? '停止' : '连接');
  const mainHint = () => (linkUp() ? '连接' : broadcasting() ? '广播' : '主机');
  const mainPress = () => (busy() ? disconnect() : connect());
  const mainClass = () => (busy() ? STYLE.pairMainStop : STYLE.pairMain);

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View class="w-full flex-col items-center pt-[38]">
        <View class="w-full h-[40] flex-row items-center justify-center gap-2 shrink-0">
          {/* 页面不在画面上时不渲染 spinner：createSpriteAnimation 的逐帧推进
              仍在跑，但 Image 卸下后没有人订阅帧值，原生 setImage 与随之的
              重排只在页面可见时发生。 */}
          {props.active() && busy() ? (
            <Image class="w-[20] h-[20] shrink-0" src={spinnerSrc.value} />
          ) : null}
          <Text class="text-lg font-bold shrink-0" style={{ textColor: pairingColor(hw.pairing) }}>
            {pairingLabel(hw.pairing)}
          </Text>
        </View>
        {hw.pairingMessage !== '' ? (
          <Text
            class="text-xs mt-1 text-center shrink-0"
            style={{ textColor: COLOR.onSurfaceVariant }}
          >
            {hw.pairingMessage}
          </Text>
        ) : null}

        <View class="flex-row items-center gap-2 mt-3 shrink-0">
          <View
            focusable={props.interactive()}
            onPress={mainPress}
            class={mainClass()}
          >
            <Text
              class="text-base font-bold"
              style={{ textColor: busy() ? COLOR.onErrorContainer : COLOR.onPrimaryContainer }}
            >
              {mainLabel()}
            </Text>
            <Text
              class="text-xs"
              style={{ textColor: busy() ? COLOR.onErrorContainer : COLOR.onPrimary }}
            >
              {mainHint()}
            </Text>
          </View>
          {broadcasting() ? (
            <View
              focusable={props.interactive()}
              onPress={pressLr}
              class={STYLE.pairAux}
            >
              <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
                配对
              </Text>
              <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
                L+R
              </Text>
            </View>
          ) : (
            <View
              focusable={props.interactive()}
              onPress={startPairing}
              class={STYLE.pairAux}
            >
              <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
                配对
              </Text>
              <Text class="text-sm font-bold" style={{ textColor: COLOR.onSurface }}>
                新主机
              </Text>
            </View>
          )}
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
