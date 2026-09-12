/** 系统页：背光调节（20%–100%，防误设黑屏）、重启、设备信息。 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { hw, setBacklight } from '../hooks/useHardware';
import { COLOR } from '../theme';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
import { formatMb, formatUptime } from '../utils';

const BACKLIGHT_STEP = 20;
/** 背光滑轨实际宽度：行内 184（240 - 页面 px-4 32 - 卡片 px-3 24）
 *  - 图标 16 - 两个按钮 80 - 档位数字 8 - 四处 gap 32。 */
const TRACK_W = 48;
/** 桥接协议仍以 0–100 百分比传输背光，显示侧映射为 1–5 档位数字。 */
const BACKLIGHT_LEVELS = 5;
/** 设备信息卡高度：py-2 上下 16 + 六行 22。 */
const INFO_H = 16 + 22 * 6;

/** 信息行：标签在左，值用 grow 占位推到行尾。 */
function InfoRow(props: { label: string; value: string }) {
  return (
    <View class="w-full h-[22] shrink-0 flex-row items-center">
      <Text class="text-xs text-[#9aacca] shrink-0">{props.label}</Text>
      <View class="grow" />
      <Text class="text-xs text-[#d9e6ff] shrink-0">{props.value}</Text>
    </View>
  );
}

export function SystemPage(props: { active: () => boolean; onAskReboot: () => void }) {
  const changeBacklight = (delta: number) => {
    // 不提供 0 档：最低保持一步，避免误触后屏幕全黑看不到画面。
    setBacklight(Math.max(BACKLIGHT_STEP, hw.backlight + delta));
  };
  const backlightLevel = () =>
    Math.max(1, Math.min(BACKLIGHT_LEVELS, Math.round(hw.backlight / BACKLIGHT_STEP)));

  const scroller = usePageScroll(
    props.active,
    () => 34 + 56 + 16 + 44 + 16 + INFO_H + 16 + BOTTOM_PLACEHOLDER_H,
  );
  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] gap-4"
        style={{ translateY: -scroller.offset() }}
      >
        <View class="w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 py-2 gap-2">
          <Icon glyph={ICON.brightnessHigh} class="shrink-0 text-base" color={COLOR.primary} />
          <View
            focusable
            onPress={() => changeBacklight(-BACKLIGHT_STEP)}
            class="w-[40] h-[40] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150"
          >
            <Text class="text-lg text-[#9aacca]">−</Text>
          </View>
          <View class="grow h-[8] rounded-[4] bg-[#14263e] overflow-hidden">
            <View
              class="h-[8] rounded-[4] bg-[#9ecefe]"
              style={{ width: (hw.backlight / 100) * TRACK_W }}
            />
          </View>
          <Text class="text-xs text-[#9aacca] shrink-0">{`${backlightLevel()}`}</Text>
          <View
            focusable
            onPress={() => changeBacklight(BACKLIGHT_STEP)}
            class="w-[40] h-[40] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150"
          >
            <Text class="text-lg text-[#9aacca]">+</Text>
          </View>
        </View>

        <View
          focusable
          onPress={props.onAskReboot}
          class="w-full h-[44] shrink-0 rounded-[16] bg-[#8a1a1e] flex-row items-center justify-center gap-2 active:bg-[#a02a2e] transition-colors duration-150"
        >
          <Icon glyph={ICON.power} class="shrink-0 text-base" color="#ff9993" />
          <Text class="text-sm font-bold text-[#ff9993]">重启设备</Text>
        </View>

        <View class="w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-2">
          <InfoRow label="芯片" value={hw.chip || 'ESP32-S3'} />
          <InfoRow label="固件" value={hw.firmwareVersion || '--'} />
          <InfoRow
            label="内存"
            value={
              hw.heapSize > 0
                ? `${Math.round((hw.heapSize - hw.heapFree) / 1024)} / ${Math.round(hw.heapSize / 1024)} KB`
                : '--'
            }
          />
          <InfoRow
            label="PSRAM"
            value={hw.psramSize > 0 ? `${formatMb(hw.psramSize - hw.psramFree)} / ${formatMb(hw.psramSize)}` : '--'}
          />
          <InfoRow
            label="电池"
            value={`${hw.battery.percentage}% · ${(hw.battery.voltageMv / 1000).toFixed(2)}V${hw.battery.charging ? ' · 充电中' : ''}`}
          />
          <InfoRow label="运行时长" value={formatUptime(hw.uptimeMs)} />
        </View>

        <BottomPlaceholder />
      </View>
    </View>
  );
}
