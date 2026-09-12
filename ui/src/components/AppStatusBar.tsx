/**
 * 顶部状态栏：半透明覆盖层，absolute 压在页面之上，内容从其下滚过。
 * 运行时没有 backdrop blur 能力，只能靠底色 alpha 弱化透出的内容。
 * 图标来自 Material Icons 字体烘焙（icons.tsx）。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { COLOR, STYLE } from '../theme';
import { hw } from '../hooks/useHardware';
import { usbLinkState, usbRoleLabel } from '../utils';

export function AppStatusBar() {
  const usbOn = usbLinkState(hw.usbRole, hw.usbRoleActive) !== 'off';
  const btConnected = hw.pairing === 'connected';
  return (
    <View class={STYLE.statusBar}>
      <Icon
        glyph={usbOn ? ICON.usb : ICON.usbOff}
        class="shrink-0 text-sm"
        color={usbOn ? COLOR.primary : COLOR.onSurfaceVariant}
      />
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {usbRoleLabel(hw.usbRole)}
      </Text>
      <View class="grow" />
      <Icon
        glyph={btConnected ? ICON.bluetoothConnected : ICON.bluetoothDisabled}
        class="shrink-0 text-sm"
        color={btConnected ? COLOR.primary : COLOR.onSurfaceVariant}
      />
      <Icon
        glyph={hw.battery.charging ? ICON.batteryChargingFull : ICON.batteryStd}
        class="shrink-0 text-sm"
        color={COLOR.onSurfaceVariant}
      />
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {`${hw.battery.percentage}%`}
      </Text>
    </View>
  );
}
