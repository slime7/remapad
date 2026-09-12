/**
 * Material Icons 字形与图标组件。
 *
 * 图标走字体烘焙：MaterialIcons-Regular.ttf 在 fonts.json 里注册为回退字体面，
 * PUA 码点只出现在本文件的字符串字面量里，构建期按需烘焙进各字号槽位；
 * 渲染为单色文本，颜色用 textColor 控制。字号只能用 Tailwind 槽位
 * （xs 12 / sm 14 / base 16 / lg 18 / xl 20 / 2xl 24）。
 */
import { Text } from '@pocketjs/framework/vue-vapor/components';
import { COLOR } from './theme';

export const ICON = {
  home: '\ue88a',
  settings: '\ue8b8',
  bluetooth: '\ue1a7',
  bluetoothConnected: '\ue1a8',
  bluetoothDisabled: '\ue1a9',
  usb: '\ue1e0',
  usbOff: '\ue4fa',
  adb: '\ue60e',
  computer: '\ue30a',
  gamepad: '\ue30f',
  chevronRight: '\ue5cc',
  power: '\ue8ac',
  swapHoriz: '\ue8d4',
  bug: '\ue868',
  brightnessHigh: '\ue1ac',
  batteryStd: '\ue1a5',
  batteryChargingFull: '\ue1a3',
} as const;

interface IconProps {
  glyph: string;
  /** 完整 class 字面量（含字号槽位与 shrink-0），在调用处声明以便扫描烘焙。 */
  class: string;
  color?: string;
}

export function Icon(props: IconProps) {
  return (
    <Text
      class={props.class}
      style={{ textColor: props.color ?? COLOR.onSurface, translateY: 2 }}
    >
      {props.glyph}
    </Text>
  );
}
