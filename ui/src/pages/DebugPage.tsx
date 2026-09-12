/**
 * 调试页：分区承载各类测试工具，当前只有「按键指令」区——把单次按键
 * 注入数据面（约 250ms 后释放），经合成输入源叠加进 BLE 输入报文，
 * 用于实机手动验证上报链路。后续测试区在滚动列里继续向下追加。
 */
import { ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { usePageScroll } from '../hooks/usePageScroll';
import { sendDebugKey } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
import type { DebugKey } from '../bridge/protocol';

/** 注入确认高亮的持续帧数：60Hz 下 9 帧约 150ms。 */
const FLASH_TICKS = 9;
/** 按键指令卡高度：py-3 上下 24 + 说明行 15 + mt-2 8 + 按钮行 44。 */
const KEYS_CARD_H = 24 + 15 + 8 + 44;

export function DebugPage(props: { active: () => boolean }) {
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

  const scroller = usePageScroll(
    props.active,
    () => 34 + 15 + 8 + KEYS_CARD_H + 8 + BOTTOM_PLACEHOLDER_H,
  );

  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        <Text class="text-xs text-[#9aacca] shrink-0">按键指令</Text>
        <View class="w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-3">
          <Text class="text-xs text-[#9aacca]">单次注入约 250ms</Text>
          <View class="w-full flex-row gap-2 mt-2">
            <View
              focusable
              onPress={() => press('a')}
              class={
                flashKey.value === 'a'
                  ? 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150'
                  : 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150'
              }
            >
              <Text
                class={
                  flashKey.value === 'a'
                    ? 'text-sm font-bold text-[#04456e]'
                    : 'text-sm font-bold text-[#d9e6ff]'
                }
              >
                A
              </Text>
            </View>
            <View
              focusable
              onPress={() => press('home')}
              class={
                flashKey.value === 'home'
                  ? 'grow h-[44] rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150'
                  : 'grow h-[44] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150'
              }
            >
              <Text
                class={
                  flashKey.value === 'home'
                    ? 'text-sm font-bold text-[#04456e]'
                    : 'text-sm font-bold text-[#d9e6ff]'
                }
              >
                唤醒 HOME
              </Text>
            </View>
          </View>
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
