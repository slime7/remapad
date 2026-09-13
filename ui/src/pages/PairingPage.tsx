/**
 * 配对页：状态流转 + 大号开始/停止按钮 + 配对 L+R 确认按钮。
 * Pro 手柄部分注册界面需要按下 L+R 确认；JoyCon 组合以左右同时连为准，
 * 该按钮触发固件左右双机配对。内容单屏放得下，静态页不挂滚动。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { hw, pressLr, sendDebugKey, startPairing, stopPairing } from '../hooks/useHardware';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { pairingColor, pairingLabel } from '../utils';

const SPINNER_FRAMES = [
  'spinner-00.svg',
  'spinner-01.svg',
  'spinner-02.svg',
  'spinner-03.svg',
  'spinner-04.svg',
  'spinner-05.svg',
  'spinner-06.svg',
  'spinner-07.svg',
];

export function PairingPage() {
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: 3 });
  // Vue Vapor：条件以函数形式在 JSX 内调用才会被渲染作用跟踪。
  const isBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  return (
    <View class="w-full h-full overflow-hidden">
      <View class="w-full flex-col items-center pt-[38]">
        <View class="w-full h-[40] flex-row items-center justify-center gap-2 shrink-0">
          {isBusy() ? <Image class="w-[20] h-[20] shrink-0" src={spinnerSrc.value} /> : null}
          <Text class="text-lg font-bold shrink-0" style={{ textColor: pairingColor(hw.pairing) }}>
            {pairingLabel(hw.pairing)}
          </Text>
        </View>
        <Text class="text-xs mt-1 text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          {hw.pairingMessage || (isBusy() ? '在 NS2 主机配对界面搜索本设备' : '')}
        </Text>

        <View class="flex-row items-center gap-2 mt-3 shrink-0">
          <View
            focusable
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
              focusable
              onPress={() =>
                hw.controllerConfig.type === 'joycon' ? pressLr() : sendDebugKey('lr')
              }
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
