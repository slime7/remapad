/** 配对页：状态流转 + 大号开始/停止按钮。内容单屏放得下，静态页不挂滚动。 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { hw, startPairing, stopPairing } from '../hooks/useHardware';
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
        <Text class="text-xs text-[#9aacca] mt-1 text-center shrink-0">
          {hw.pairingMessage || (isBusy() ? '在 NS2 主机配对界面搜索本设备' : '')}
        </Text>

        <View
          focusable
          onPress={isBusy() ? stopPairing : startPairing}
          class={
            isBusy()
              ? 'w-[76] h-[76] rounded-full bg-[#8a1a1e] flex-col items-center justify-center mt-3 active:bg-[#a02a2e] transition-colors duration-150'
              : 'w-[76] h-[76] rounded-full bg-[#9ecefe] flex-col items-center justify-center mt-3 active:bg-[#b8dbff] transition-colors duration-150'
          }
        >
          <Text
            class={
              isBusy()
                ? 'text-base font-bold text-[#ff9993]'
                : 'text-base font-bold text-[#04456e]'
            }
          >
            {isBusy() ? '停止' : '开始'}
          </Text>
          <Text
            class={
              isBusy() ? 'text-xs text-[#ff9993]' : 'text-xs text-[#164e77]'
            }
          >
            配对
          </Text>
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
