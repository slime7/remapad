/** 系统页：背光调节（20%–100%，防误设黑屏）、重启、设备信息。 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setBacklight } from '../hooks/useHardware';
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
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {props.label}
      </Text>
      <View class="grow" />
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurface }}>
        {props.value}
      </Text>
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
        <View class={STYLE.backlightRow}>
          <Icon glyph={ICON.brightnessHigh} class="shrink-0 text-base" color={COLOR.primary} />
          <View focusable onPress={() => changeBacklight(-BACKLIGHT_STEP)} class={STYLE.surfaceBtn}>
            <Text class="text-lg" style={{ textColor: COLOR.onSurfaceVariant }}>
              −
            </Text>
          </View>
          <View class={STYLE.track}>
            <View
              class={STYLE.trackFill}
              style={{ width: (hw.backlight / 100) * TRACK_W }}
            />
          </View>
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
            {`${backlightLevel()}`}
          </Text>
          <View focusable onPress={() => changeBacklight(BACKLIGHT_STEP)} class={STYLE.surfaceBtn}>
            <Text class="text-lg" style={{ textColor: COLOR.onSurfaceVariant }}>
              +
            </Text>
          </View>
        </View>

        <View focusable onPress={props.onAskReboot} class={STYLE.dangerRow}>
          <Icon glyph={ICON.power} class="shrink-0 text-base" color={COLOR.onErrorContainer} />
          <Text class="text-sm font-bold" style={{ textColor: COLOR.onErrorContainer }}>
            重启设备
          </Text>
        </View>

        <View class={STYLE.infoCard}>
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
