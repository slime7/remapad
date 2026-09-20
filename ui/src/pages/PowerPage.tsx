/**
 * 电源管理页（第 4 页）：
 * 页根铺满整张 256 卡片，两枚与背景瓣外弧同心的 64 圆钮斜角放置：
 * 左上「重启设备」、右下「设备关机」（error 语义色），点击唤起确认对话框。
 * 四字标签拆两行展示（框架单行 Text 不换行），上下方向键按焦点环顺序切换。
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
    <View class={props.active() ? 'relative w-full h-full' : 'hidden'}>
      {/* 重启设备按钮（左上瓣心，两行标签） */}
      <View
        focusable={props.interactive()}
        onPress={props.onAskReboot}
        class={STYLE.cornerBtnTL}
      >
        <View class="flex-col items-center justify-center gap-[2]">
          <Text class="text-xs font-bold" style={{ textColor: COLOR.onSecondaryContainer }}>
            重启
          </Text>
          <Text class="text-xs font-bold" style={{ textColor: COLOR.onSecondaryContainer }}>
            设备
          </Text>
        </View>
      </View>

      {/* 设备关机按钮（右下瓣心，error 语义色，两行标签） */}
      <View
        focusable={props.interactive()}
        onPress={props.onAskPowerOff}
        class={STYLE.cornerBtnBRError}
      >
        <View class="flex-col items-center justify-center gap-[2]">
          <Text class="text-xs font-bold" style={{ textColor: COLOR.onErrorContainer }}>
            设备
          </Text>
          <Text class="text-xs font-bold" style={{ textColor: COLOR.onErrorContainer }}>
            关机
          </Text>
        </View>
      </View>
    </View>
  );
}
