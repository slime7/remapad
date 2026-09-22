/**
 * Remapad 底部状态栏组件（224 × 64）：
 * 位于屏幕底部 (x:8, y:208, w:224, h:64)，上圆角 8px，下圆角 32px。
 *
 * 状态优先级：
 * 1. OTA 数据接收进度条（最高）
 * 2. 手柄操控屏幕提示模式（两行提示文本）
 * 3. 三等分状态栏（默认）：物理手柄状态、主机连接状态（带玩家指示灯，未连接点击触发信号搜索）、电池电量。
 */
import { ref } from 'vue';
import { View, Text, Image } from '@pocketjs/framework/vue-vapor/components';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { COLOR, STYLE } from '../theme';
import { ICON, Icon } from '../icons';
import { connect, hw } from '../hooks/useHardware';

const OTA_TRACK_W = 192;

/** 微型玩家序号指示灯（单格 8×8，间距 3px）。 */
function MiniPlayerLedRow(props: { mask: () => number }) {
  return (
    <View class="flex-row gap-[3] shrink-0 items-center justify-center">
      <View class={(props.mask() & 0b0001) !== 0 ? STYLE.miniPlayerLedOn : STYLE.miniPlayerLedOff} />
      <View class={(props.mask() & 0b0010) !== 0 ? STYLE.miniPlayerLedOn : STYLE.miniPlayerLedOff} />
      <View class={(props.mask() & 0b0100) !== 0 ? STYLE.miniPlayerLedOn : STYLE.miniPlayerLedOff} />
      <View class={(props.mask() & 0b1000) !== 0 ? STYLE.miniPlayerLedOn : STYLE.miniPlayerLedOff} />
    </View>
  );
}

export function BottomBar() {
  const isOta = () => hw.ota.phase === 'receiving';
  const isPadUi = () => hw.padUiMode;

  // 手柄状态：tertiary 背景上已连接使用高强调 onTertiary，未连接使用弱化 onTertiaryContainer
  const padAttached = () => hw.physicalPad.attached;
  const padGlyph = () => (padAttached() ? ICON.videogameAsset : ICON.videogameAssetOff);
  const padColor = () => (padAttached() ? COLOR.onTertiary : COLOR.onTertiaryContainer);
  const padLabel = () => (padAttached() ? (hw.physicalPad.name || 'PRO') : '未连接');

  // 主机连接状态
  const hostConnected = () => hw.pairing === 'connected';
  const isSearching = () => hw.pairing === 'advertising' || hw.pairing === 'scanning';
  const hostColor = () => (hostConnected() ? COLOR.onTertiary : COLOR.onTertiaryContainer);

  const hostIconOpacity = ref(1);
  let breathTick = 0;
  const BREATH_CYCLE = 36; // 30Hz 下 1.2 秒一个呼吸周期

  onFrame(() => {
    if (isSearching()) {
      breathTick = (breathTick + 1) % BREATH_CYCLE;
      // 0..1..0 平滑正弦波
      const wave = (1 - Math.cos((2 * Math.PI * breathTick) / BREATH_CYCLE)) * 0.5;
      // 呼吸透明度在 0.25 到 1.0 之间渐变
      hostIconOpacity.value = Math.round((0.25 + wave * 0.75) * 100) / 100;
    } else if (hostIconOpacity.value !== 1) {
      hostIconOpacity.value = 1;
      breathTick = 0;
    }
  });

  // 电池电量状态
  const batteryPct = () => hw.battery.percentage;
  const batteryGlyph = () => {
    const pct = batteryPct();
    if (pct <= 15) {
      return ICON.batteryAlert;
    }
    if (pct >= 90) {
      return ICON.batteryFull;
    }
    return ICON.batteryStd;
  };
  const batteryColor = () => {
    const pct = batteryPct();
    if (pct <= 15) {
      return COLOR.onError;
    }
    return COLOR.onTertiary;
  };

  const onHostClick = () => {
    if (hw.pairing === 'idle' || hw.pairing === 'paired') {
      connect();
    }
  };

  return (
    <View class="absolute left-[8] right-[8] bottom-[8] h-[64] z-40 overflow-hidden">
      {/* 底部异形圆角矢量背景贴图（256×64，两边各留 16px 透明边距，背景为 tertiary #4eb079） */}
      <Image src="dock.svg" class="absolute left-[-16] top-0 w-[256] h-[64]" />

      {/* 优先级 1：OTA 进度条 */}
      <View class={isOta() ? 'w-full h-full flex-col items-center justify-center px-4 gap-2' : 'hidden'}>
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>
          {`OTA 接收中: ${hw.ota.percentage}%`}
        </Text>
        <View class={STYLE.track}>
          <View
            class={STYLE.trackFill}
            style={{ width: Math.round((Math.max(0, Math.min(100, hw.ota.percentage)) / 100) * OTA_TRACK_W) }}
          />
        </View>
      </View>

      {/* 优先级 2：手柄操控提示 */}
      <View class={!isOta() && isPadUi() ? 'w-full h-full flex-col items-center justify-center px-2 py-1 gap-1' : 'hidden'}>
        <View class="flex-row items-center gap-1">
          <Icon glyph={ICON.gamepadLeft} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Icon glyph={ICON.gamepadRight} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Icon glyph={ICON.gameButtonL} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Icon glyph={ICON.gameButtonR} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>翻页</Text>
          <View class="w-2" />
          <Icon glyph={ICON.gamepadUp} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Icon glyph={ICON.gamepadDown} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>选择</Text>
        </View>
        <View class="flex-row items-center gap-1">
          <Icon glyph={ICON.gamepadCircleRight} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>确认</Text>
          <View class="w-2" />
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>长按</Text>
          <Icon glyph={ICON.gamepadCircleDown} class="text-sm shrink-0" color={COLOR.onTertiary} />
          <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>退出</Text>
        </View>
      </View>

      {/* 优先级 3：三等分状态显示（上行图标 h-[24]、下行状态 h-[18]，确保三列图标严格水平对齐） */}
      <View class={!isOta() && !isPadUi() ? 'w-full h-full flex-row items-center px-1' : 'hidden'}>
        {/* 左区：物理手柄 */}
        <View class="grow basis-0 h-full flex-col items-center justify-center gap-1">
          <View class="h-[24] flex-row items-center justify-center">
            <Icon glyph={padGlyph()} class="text-xl shrink-0" color={padColor()} />
          </View>
          <View class="h-[18] flex-row items-center justify-center">
            <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>
              {padLabel()}
            </Text>
          </View>
        </View>

        {/* 中区：主机连接 + 玩家指示灯（仅供点击触发，不参与手柄焦点导航） */}
        <View
          focusable
          onPress={onHostClick}
          class="grow basis-0 h-full flex-col items-center justify-center gap-1 rounded-[8] active:bg-[#1a8552]"
        >
          <View
            class="h-[24] flex-row items-center justify-center"
            style={{ opacity: hostIconOpacity.value }}
          >
            <Icon glyph={ICON.missingController} class="text-xl shrink-0" color={hostColor()} />
          </View>
          <View class="h-[18] flex-row items-center justify-center">
            <MiniPlayerLedRow mask={() => hw.playerLed} />
          </View>
        </View>

        {/* 右区：设备电量 */}
        <View class="grow basis-0 h-full flex-col items-center justify-center gap-1">
          <View class="h-[24] flex-row items-center justify-center">
            <Icon glyph={batteryGlyph()} class="text-xl shrink-0" color={batteryColor()} />
          </View>
          <View class="h-[18] flex-row items-center justify-center">
            <Text class="text-xs shrink-0" style={{ textColor: COLOR.onTertiary }}>
              {`${batteryPct()}%`}
            </Text>
          </View>
        </View>
      </View>
    </View>
  );
}
