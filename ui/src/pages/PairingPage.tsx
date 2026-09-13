/**
 * 配对页：状态流转 + 大号开始/停止按钮 + 配对 L+R 确认按钮。
 * 设备开机即按凭证回连或发发现广播，配对不需要打开本页；本页只负责手动
 * 开关发现广播与按下确认键。JoyCon 是左右两只各自独立连接，主机的 Grip/
 * 顺序界面只用于调整顺序与确认，因此确认按钮触发固件左右双机配对，Pro 则
 * 注入 L+R 按键。内容单屏放得下，静态页不挂滚动。页面按需挂载，切页由
 * 根节点翻转 hidden 完成（见 docs/adr/0014-page-mount-on-demand-progressive-fill.md）。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { createSpriteAnimation } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder } from '../components/BottomPlaceholder';
import { hw, pressLr, sendDebugKey, startPairing, stopPairing } from '../hooks/useHardware';
import { SPINNER_FRAMES } from '../spinner';
import { pairingColor, pairingLabel } from '../utils';

export function PairingPage(props: { active: () => boolean }) {
  const spinnerSrc = createSpriteAnimation(SPINNER_FRAMES, { frameStep: 3 });
  // Vue Vapor：条件以函数形式在 JSX 内调用才会被渲染作用跟踪。
  const isBusy = () => hw.pairing === 'scanning' || hw.pairing === 'pairing';

  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View class="w-full flex-col items-center pt-[38]">
        <View class="w-full h-[40] flex-row items-center justify-center gap-2 shrink-0">
          {isBusy() ? <Image class="w-[20] h-[20] shrink-0" src={spinnerSrc.value} /> : null}
          <Text class="text-lg font-bold shrink-0" style={{ textColor: pairingColor(hw.pairing) }}>
            {pairingLabel(hw.pairing)}
          </Text>
        </View>
        <Text class="text-xs mt-1 text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          {hw.pairingMessage ||
            (isBusy() ? '在 NS2 主机配对界面搜索本设备' : '配对无需打开本页，开机即自动广播')}
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
        {isBusy() ? (
          <Text class="text-xs mt-1 text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
            对应主机 Grip 界面确认
          </Text>
        ) : null}
        <BottomPlaceholder />
      </View>
    </View>
  );
}
