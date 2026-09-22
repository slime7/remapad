/**
 * 系统信息页：
 * 位于四叶草中心区域 (130 × 130)。
 * 布局：紧凑展示固件版本、堆内存、PSRAM 及电池电压核心数据。
 * 无滚动，固定单屏居中展示。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR } from '../theme';
import { hw } from '../hooks/useHardware';

export function SystemInfoPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const fwVer = () => hw.firmwareVersion || 'v0.4.0';
  /** 桥接上报的是可用量，两行都按「已用 / 总共」上屏。 */
  const heapText = () => {
    const used = Math.max(0, hw.heapSize - hw.heapFree);
    return `${(used / 1024).toFixed(0)} / ${(hw.heapSize / 1024).toFixed(0)} KB`;
  };
  const psramText = () => {
    if (hw.psramSize <= 0) {
      return '--';
    }
    const used = Math.max(0, hw.psramSize - hw.psramFree);
    return `${(used / (1024 * 1024)).toFixed(1)} / ${(hw.psramSize / (1024 * 1024)).toFixed(0)} MB`;
  };
  const batteryText = () => `${hw.battery.percentage}% · ${(hw.battery.voltageMv / 1000).toFixed(2)}V`;

  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-2 gap-[6]' : 'hidden'}>
      <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
        设备信息
      </Text>
      <View class="flex-col items-start gap-1 shrink-0 w-full px-1">
        <Text class="text-xs shrink-0 font-bold" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`固件: ${fwVer()}`}
        </Text>
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`堆内存: ${heapText()}`}
        </Text>
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`PSRAM: ${psramText()}`}
        </Text>
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
          {`电池: ${batteryText()}`}
        </Text>
      </View>
    </View>
  );
}
