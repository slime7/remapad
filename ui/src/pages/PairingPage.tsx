/**
 * 配对页：状态流转 + 大号开始/停止按钮 + 配对 L+R 确认按钮。
 * 「开始」等价于真机按住配对键：固件先断开当前主机，再发发现广播等新主机
 * 搜索（见 ADR 0024）；不按它时设备完全自动——配过主机就常驻唤醒广播等
 * 主机回连，主机停在任意页面都能连上，从未配过则开机自动进入本流程。配对
 * 成功由固件自动退出配对流程，不需要按停止。JoyCon 是左右两只各自独立
 * 连接，主机的 Grip/顺序界面只用于调整顺序与确认；两只未配对期间固件会
 * 自动注入 L+R 120ms 并每 3 秒重试，本页按钮是手动兜底（JoyCon 触发左右
 * 双机配对，Pro 注入 L+R 按键）。内容单屏放得下，静态页不挂滚动。七个页面在
 * 首屏前一次挂完，切页由
 * 根节点翻转 hidden 完成（见 docs/adr/0016-mount-all-pages-before-first-frame.md）。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { hw, pressLr, startPairing, stopPairing } from '../hooks/useHardware';
import { SPINNER_FRAMES } from '../spinner';
import { pairingColor, pairingLabel } from '../utils';

export function PairingPage(props: {
  active: () => boolean;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: 3 });
  // Vue Vapor：条件以函数形式在 JSX 内调用才会被渲染作用跟踪。
  const isBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View class="w-full flex-col items-center pt-[38]">
        <View class="w-full h-[40] flex-row items-center justify-center gap-2 shrink-0">
          {/* 页面不在画面上时不渲染 spinner：createSpriteAnimation 的逐帧推进
              仍在跑，但 Image 卸下后没有人订阅帧值，原生 setImage 与随之的
              重排只在页面可见时发生。 */}
          {props.active() && isBusy() ? (
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
            onPress={isBusy() ? stopPairing : startPairing}
            class={isBusy() ? STYLE.pairMainStop : STYLE.pairMain}
          >
            <Text
              class="text-base font-bold"
              style={{ textColor: isBusy() ? COLOR.onErrorContainer : COLOR.onPrimaryContainer }}
            >
              {isBusy() ? '停止' : '开始'}
            </Text>
            <Text
              class="text-xs"
              style={{ textColor: isBusy() ? COLOR.onErrorContainer : COLOR.onPrimary }}
            >
              配对
            </Text>
          </View>
          {isBusy() ? (
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
          ) : null}
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
