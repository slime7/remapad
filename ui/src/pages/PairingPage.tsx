/**
 * 配对页（第 3 页）：
 * 位于四叶草中心区域 (130 × 130)。
 * 布局：
 * - 上方：配对状态（文字与广播时的 Spinner 动画）；
 * - 中央：连接/停止/断开主按钮；
 * - 下方：配对新主机按钮。
 * 上下方向键可在主副按钮间切换焦点。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { connect, disconnect, hw, startPairing } from '../hooks/useHardware';
import { SPINNER_FRAMES, SPINNER_STEP } from '../spinner';
import { pairingLabel } from '../utils';

export function PairingPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: SPINNER_STEP });
  const broadcasting = () => hw.pairing === 'scanning' || hw.pairing === 'advertising';
  const linkUp = () => hw.pairing === 'pairing' || hw.pairing === 'connected';
  const busy = () => broadcasting() || linkUp();
  const mainLabel = () => (linkUp() ? '断开' : broadcasting() ? '停止' : '连接');
  const mainPress = () => (busy() ? disconnect() : connect());
  const mainClass = () => (busy() ? STYLE.pairStopBtn : STYLE.pairMainBtn);

  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-2 gap-2' : 'hidden'}>
      {/* 状态文字与 Spinner */}
      <View class="flex-col items-center justify-center shrink-0">
        <View class="flex-row items-center justify-center gap-1 shrink-0 h-[18]">
          {props.active() && busy() ? (
            <Image class="w-[14] h-[14] shrink-0" src={spinnerSrc.value} />
          ) : null}
          <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
            {pairingLabel(hw.pairing)}
          </Text>
        </View>
        {hw.pairingMessage !== '' ? (
          <Text class="text-xs text-center shrink-0 mt-[2]" style={{ textColor: COLOR.onPrimaryContainer }}>
            {hw.pairingMessage}
          </Text>
        ) : null}
      </View>

      {/* 主连接按钮 */}
      <View
        focusable={props.interactive()}
        onPress={mainPress}
        class={mainClass()}
      >
        <Text
          class="text-xs font-bold"
          style={{ textColor: busy() ? COLOR.onErrorContainer : COLOR.primaryContainer }}
        >
          {mainLabel()}
        </Text>
      </View>

      {/* 副配对按钮 */}
      <View
        focusable={props.interactive()}
        onPress={startPairing}
        class={STYLE.pairAuxBtn}
      >
        <Text class="text-xs" style={{ textColor: COLOR.onSurface }}>
          配对新主机
        </Text>
      </View>
    </View>
  );
}
