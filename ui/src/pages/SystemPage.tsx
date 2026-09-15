/** 系统页：背光调节（20%–100%，防误设黑屏）、重启与关机、设备信息与实时帧率。 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { watchEffect } from 'vue';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setBacklight, setFrameRateSampling } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PAD_H } from '../components/BottomPlaceholder';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';
import { formatMb, formatUptime } from '../utils';

const BACKLIGHT_STEP = 20;
/** 背光滑轨实际宽度：行内 184（240 - 页面 px-4 32 - 卡片 px-3 24）
 *  - 图标 16 - 两个按钮 80 - 档位数字 8 - 四处 gap 32。 */
const TRACK_W = 48;
/** 桥接协议仍以 0–100 百分比传输背光，显示侧映射为 1–5 档位数字。 */
const BACKLIGHT_LEVELS = 5;
/** 设备信息卡高度：py-2 上下 16 + 七行 22。 */
const INFO_H = 16 + 22 * 7;
/** 内容高度公式与焦点行位置共用：顶部留白、背光行高、块间距与操作行高。 */
const TOP_PAD = 34;
const BACKLIGHT_H = 56;
const GAP = 16;
const ACTION_H = 44;

/** 信息行：标签在左、值在右，用 justify-between 顶开（省掉一个占位节点）。 */
function InfoRow(props: { label: string; value: string }) {
  return (
    <View class="w-full h-[22] shrink-0 flex-row items-center justify-between">
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
        {props.label}
      </Text>
      <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurface }}>
        {props.value}
      </Text>
    </View>
  );
}

export function SystemPage(props: {
  active: () => boolean;
  onAskReboot: () => void;
  onAskPowerOff: () => void;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
  interactive: () => boolean;
}) {
  const changeBacklight = (delta: number) => {
    // 不提供 0 档：最低保持一步，避免误触后屏幕全黑看不到画面。
    setBacklight(Math.max(BACKLIGHT_STEP, hw.backlight + delta));
  };
  const backlightLevel = () =>
    Math.max(1, Math.min(BACKLIGHT_LEVELS, Math.round(hw.backlight / BACKLIGHT_STEP)));

  // 帧率只在系统页可见时采样：离页停止，不为看不见的数字持续读设备状态。
  watchEffect(() => {
    setFrameRateSampling(props.active());
  });

  /* 可聚焦行的位置：背光 −/+ 同处一行，下面是重启与关机两行。 */
  const rowNodes: Array<NodeMirror | null> = [];
  const focusRows = () => [
    { node: rowNodes[0] ?? null, y: TOP_PAD, h: BACKLIGHT_H },
    { node: rowNodes[1] ?? null, y: TOP_PAD, h: BACKLIGHT_H },
    { node: rowNodes[2] ?? null, y: TOP_PAD + BACKLIGHT_H + GAP, h: ACTION_H },
    { node: rowNodes[3] ?? null, y: TOP_PAD + BACKLIGHT_H + GAP * 2 + ACTION_H, h: ACTION_H },
  ];
  const contentRef = usePageScroll(
    props.active,
    true,
    () => TOP_PAD + BACKLIGHT_H + GAP * 3 + ACTION_H * 2 + INFO_H + BOTTOM_PAD_H,
    focusRows,
  );
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View nodeRef={contentRef} class="w-full flex-col px-4 pt-[34] gap-4">
        <View class={STYLE.backlightRow}>
            <Icon glyph={ICON.brightnessHigh} class="shrink-0 text-base" color={COLOR.primary} />
            <View
              nodeRef={(node: NodeMirror | null) => (rowNodes[0] = node)}
              focusable={props.interactive()}
              onPress={() => changeBacklight(-BACKLIGHT_STEP)}
              class={STYLE.surfaceBtn}
            >
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
            <View
              nodeRef={(node: NodeMirror | null) => (rowNodes[1] = node)}
              focusable={props.interactive()}
              onPress={() => changeBacklight(BACKLIGHT_STEP)}
              class={STYLE.surfaceBtn}
            >
              <Text class="text-lg" style={{ textColor: COLOR.onSurfaceVariant }}>
                +
              </Text>
            </View>
        </View>

        <View
          nodeRef={(node: NodeMirror | null) => (rowNodes[2] = node)}
          focusable={props.interactive()}
          onPress={props.onAskReboot}
          class={STYLE.dangerRow}
        >
            <Icon glyph={ICON.power} class="shrink-0 text-base" color={COLOR.onErrorContainer} />
            <Text class="text-sm font-bold" style={{ textColor: COLOR.onErrorContainer }}>
              重启设备
            </Text>
        </View>

        <View
          nodeRef={(node: NodeMirror | null) => (rowNodes[3] = node)}
          focusable={props.interactive()}
          onPress={props.onAskPowerOff}
          class={STYLE.dangerRow}
        >
            <Icon glyph={ICON.power} class="shrink-0 text-base" color={COLOR.onErrorContainer} />
            <Text class="text-sm font-bold" style={{ textColor: COLOR.onErrorContainer }}>
              关机
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
          <InfoRow label="FPS" value={hw.fps === null ? '--' : hw.fps.toFixed(1)} />
        </View>

        <BottomPlaceholder />
      </View>
    </View>
  );
}
