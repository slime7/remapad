/**
 * 电源管理页（第 4 页）：
 * 位于四叶草中心区域 (130 × 130)。
 * 布局：垂直排列的两枚操作按钮（重启设备、设备关机），点击唤起确认对话框。
 * 上下方向键可在两枚按钮间切换焦点。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';

export function PowerPage(props: {
  active: () => boolean;
  interactive: () => boolean;
  onAskReboot: () => void;
  onAskPowerOff: () => void;
}) {
  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-2 gap-3' : 'hidden'}>
      {/* 重启设备按钮 */}
      <View
        focusable={props.interactive()}
        onPress={props.onAskReboot}
        class={STYLE.powerBtn}
      >
        <Text class="text-xs font-bold" style={{ textColor: COLOR.onSurface }}>
          重启设备
        </Text>
      </View>

      {/* 设备关机按钮 */}
      <View
        focusable={props.interactive()}
        onPress={props.onAskPowerOff}
        class={STYLE.powerDangerBtn}
      >
        <Text class="text-xs font-bold" style={{ textColor: COLOR.onErrorContainer }}>
          设备关机
        </Text>
      </View>
    </View>
  );
}
