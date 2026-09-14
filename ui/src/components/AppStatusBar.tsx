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

/** 状态栏电量分档阈值：低于 15% 转告警色，90% 以上用实心电池。 */
const BATTERY_LOW_PCT = 15;
const BATTERY_FULL_PCT = 90;

/** 电量图标与颜色：充电优先（板载无充电状态引脚，该标志由固件按电压趋势推断），
 *  其余按电量分档，低电量转告警色。 */
function batteryAppearance(percentage: number, charging: boolean): { glyph: string; color: string } {
  if (charging) {
    return { glyph: ICON.batteryChargingFull, color: COLOR.primary };
  }
  if (percentage <= BATTERY_LOW_PCT) {
    return { glyph: ICON.batteryAlert, color: COLOR.error };
  }
  if (percentage >= BATTERY_FULL_PCT) {
    return { glyph: ICON.batteryFull, color: COLOR.onSurfaceVariant };
  }
  return { glyph: ICON.batteryStd, color: COLOR.onSurfaceVariant };
}

export function AppStatusBar() {
  /* Vue Vapor：派生值包成函数、在 JSX 内调用才会被渲染作用跟踪。在组件体里
   * 算成常量会被冻在首帧——设备上首帧早于固件应答，电量图标曾因此停在 0%
   * 的告警色，数字却已经是真实电量（见 tests/e2e/status-bar.spec.ts）。 */
  const usbOn = () => usbLinkState(hw.usbRole, hw.usbRoleActive) !== 'off';
  const btConnected = () => hw.pairing === 'connected';
  const battery = () => batteryAppearance(hw.battery.percentage, hw.battery.charging);
  return (
    <View class={STYLE.statusBar}>
      <Icon
        glyph={usbOn() ? ICON.usb : ICON.usbOff}
        class="shrink-0 text-sm"
        color={usbOn() ? COLOR.primary : COLOR.onSurfaceVariant}
      />
      <Text class="text-xs grow" style={{ textColor: COLOR.onSurfaceVariant }}>
        {usbRoleLabel(hw.usbRole)}
      </Text>
      <Icon
        glyph={btConnected() ? ICON.bluetoothConnected : ICON.bluetoothDisabled}
        class="shrink-0 text-sm"
        color={btConnected() ? COLOR.primary : COLOR.onSurfaceVariant}
      />
      <Icon
        glyph={battery().glyph}
        class="shrink-0 text-sm"
        color={battery().color}
      />
      <Text class="text-xs shrink-0" style={{ textColor: battery().color }}>
        {`${hw.battery.percentage}%`}
      </Text>
    </View>
  );
}
