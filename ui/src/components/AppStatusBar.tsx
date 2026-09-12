/**
 * 顶部状态栏：半透明覆盖层，absolute 压在页面之上，内容从其下滚过。
 * 运行时没有 backdrop blur 能力，只能靠底色 alpha 弱化透出的内容。
 * 图标来自 Material Icons 字体烘焙（icons.tsx）。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { hw } from '../hooks/useHardware';
import { COLOR } from '../theme';
import { usbLinkState, usbRoleLabel } from '../utils';

const MUTED = COLOR.onSurfaceVariant;

export function AppStatusBar() {
  const usbOn = usbLinkState(hw.usbRole, hw.usbRoleActive) !== 'off';
  const btConnected = hw.pairing === 'connected';
  return (
    <View class="absolute top-0 left-0 right-0 h-[26] z-40 flex-row items-center px-6 gap-2 bg-[#081423b3]">
      <Icon
        glyph={usbOn ? ICON.usb : ICON.usbOff}
        class="shrink-0 text-sm"
        color={usbOn ? COLOR.primary : MUTED}
      />
      <Text class="text-xs text-[#9aacca] shrink-0">{usbRoleLabel(hw.usbRole)}</Text>
      <View class="grow" />
      <Icon
        glyph={btConnected ? ICON.bluetoothConnected : ICON.bluetoothDisabled}
        class="shrink-0 text-sm"
        color={btConnected ? COLOR.primary : MUTED}
      />
      <Icon
        glyph={hw.battery.charging ? ICON.batteryChargingFull : ICON.batteryStd}
        class="shrink-0 text-sm"
        color={MUTED}
      />
      <Text class="text-xs text-[#9aacca] shrink-0">{`${hw.battery.percentage}%`}</Text>
    </View>
  );
}
