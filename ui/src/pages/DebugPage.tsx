/**
 * 调试页（第 6 页）：
 * 位于四叶草中心区域 (130 × 130)。
 * 条件编译页面：仅在构建传入 dev 选项时编入。
 * 布局：
 * - 第一行：A 键与 HOME 键注入测试按钮；
 * - 第二行：手柄控屏模式注入开关。
 * 注入成功后提供 150ms 短亮反馈。
 */
import { ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';
import { hw, sendDebugKey } from '../hooks/useHardware';
import { ticksForMs } from '../tick';
import type { DebugKey } from '../bridge/protocol';

const FLASH_TICKS = Math.max(1, Math.round(ticksForMs(150)));

export function DebugPage(props: {
  active: () => boolean;
  interactive: () => boolean;
}) {
  const flashKey = ref<DebugKey | null>(null);
  let flashTicks = 0;

  const press = (key: DebugKey) => {
    sendDebugKey(key, () => {
      flashKey.value = key;
      flashTicks = 0;
    });
  };

  onFrame(() => {
    if (flashKey.value === null) {
      return;
    }
    flashTicks++;
    if (flashTicks >= FLASH_TICKS) {
      flashKey.value = null;
    }
  });

  const btnClass = (key: DebugKey) => (flashKey.value === key ? STYLE.dbgBtnOn : STYLE.dbgBtn);
  const fullBtnClass = (key: DebugKey) => (flashKey.value === key ? STYLE.dbgFullBtnOn : STYLE.dbgFullBtn);
  const textColor = (key: DebugKey) =>
    flashKey.value === key ? COLOR.onPrimaryContainer : COLOR.onSurface;

  const homeLabel = () => (hw.pairing === 'connected' ? 'HOME' : '唤醒');

  return (
    <View class={props.active() ? 'w-full h-full flex-col items-center justify-center p-2 gap-2' : 'hidden'}>
      <Text class="text-xs font-bold shrink-0" style={{ textColor: COLOR.onPrimaryContainer }}>
        调试指令
      </Text>

      {/* 第一行：A 键与 HOME/唤醒 键 */}
      <View class="flex-row items-center gap-2 shrink-0">
        <View
          focusable={props.interactive()}
          onPress={() => press('a')}
          class={btnClass('a')}
        >
          <Text class="text-xs font-bold" style={{ textColor: textColor('a') }}>
            A键
          </Text>
        </View>
        <View
          focusable={props.interactive()}
          onPress={() => press('home')}
          class={btnClass('home')}
        >
          <Text class="text-xs font-bold" style={{ textColor: textColor('home') }}>
            {homeLabel()}
          </Text>
        </View>
      </View>

      {/* 第二行：手柄操控屏幕组合键注入 */}
      <View
        focusable={props.interactive()}
        onPress={() => press('ui')}
        class={fullBtnClass('ui')}
      >
        <Text class="text-xs font-bold" style={{ textColor: textColor('ui') }}>
          {hw.padUiMode ? '退出控屏' : '手柄控屏'}
        </Text>
      </View>
    </View>
  );
}
