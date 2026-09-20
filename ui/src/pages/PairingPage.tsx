/**
 * 配对页（第 3 页）：
 * 页根铺满整张 256 卡片，按钮是与背景瓣外弧同心的 64 圆钮：
 * - 上方：配对状态提示（文字与广播时的 Spinner 动画），落在四叶草上部中央；
 * - 下方两角：右下主按钮（连接/断开/停止，busy 态用 error 语义色）、
 *   左下副按钮（配对）。
 * 主副按钮在焦点环里按此顺序切换。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';
import { connect, disconnect, hw, startPairing } from '../hooks/useHardware';
import { createBrailleSpinner } from '../spinner';
import { pairingLabel } from '../utils';

export function PairingPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const spinnerChar = createBrailleSpinner();
  const broadcasting = () => hw.pairing === 'scanning' || hw.pairing === 'advertising';
  const showSpinner = () => hw.pairing === 'scanning' || hw.pairing === 'advertising' || hw.pairing === 'pairing';
  const linkUp = () => hw.pairing === 'pairing' || hw.pairing === 'connected';
  const busy = () => broadcasting() || linkUp();
  const mainLabel = () => (linkUp() ? '断开' : broadcasting() ? '停止' : '连接');
  const mainPress = () => (busy() ? disconnect() : connect());
  const mainClass = () => (busy() ? STYLE.cornerBtnBRError : STYLE.cornerBtnBR);
  const mainColor = () => (busy() ? COLOR.onErrorContainer : COLOR.onSecondaryContainer);

  return (
    <View class={props.active() ? 'relative w-full h-full' : 'hidden'}>
      {/* 状态文字与 Spinner（四叶草上部中央） */}
      <View class="absolute left-0 top-[64] w-full flex-col items-center gap-[2]">
        <View class="flex-row items-center justify-center gap-1 shrink-0 h-[18]">
          {props.active() && showSpinner() ? (
            <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
              {spinnerChar.value}
            </Text>
          ) : null}
          <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
            {pairingLabel(hw.pairing)}
          </Text>
        </View>
        {hw.pairingMessage !== '' ? (
          <Text class="text-xs text-center shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
            {hw.pairingMessage}
          </Text>
        ) : null}
      </View>

      {/* 主连接按钮（右下瓣心，停止/断开时转 error 语义色） */}
      <View
        focusable={props.interactive()}
        onPress={mainPress}
        class={mainClass()}
      >
        <Text class="text-xs font-bold" style={{ textColor: mainColor() }}>
          {mainLabel()}
        </Text>
      </View>

      {/* 副配对按钮（左下瓣心） */}
      <View
        focusable={props.interactive()}
        onPress={startPairing}
        class={STYLE.cornerBtnBL}
      >
        <Text class="text-xs" style={{ textColor: COLOR.onSecondaryContainer }}>
          配对
        </Text>
      </View>
    </View>
  );
}
