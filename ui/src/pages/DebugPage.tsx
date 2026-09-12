/**
 * 调试页：分区承载各类测试工具，当前只有「按键指令」区——把单次按键
 * 注入数据面（保持后自动释放），经合成输入源叠加进 BLE 输入报文，
 * 用于实机手动验证上报链路；「配对 L+R」模拟同时按住 L 和 R，对应主机
 * Grip/顺序界面的配对确认。后续测试区在滚动列里继续向下追加。
 */
import { ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { sendDebugKey } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
import type { DebugKey } from '../bridge/protocol';

/** 注入确认高亮的持续帧数：60Hz 下 9 帧约 150ms。 */
const FLASH_TICKS = 9;
/** 按键指令卡高度：py-3 上下 24 + 两行按钮（各 mt-2 8 + 44）。 */
const KEYS_CARD_H = 24 + 8 + 44 + 8 + 44;

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

  const keyBtnClass = (key: DebugKey, base: 'fixed' | 'grow' | 'full') => {
    const on = flashKey.value === key;
    if (base === 'grow') {
      return on ? STYLE.keyBtnGrowOn : STYLE.keyBtnGrow;
    }
    if (base === 'full') {
      return on ? STYLE.keyBtnFullOn : STYLE.keyBtnFull;
    }
    return on ? STYLE.keyBtnOn : STYLE.keyBtn;
  };
  const keyTextColor = (key: DebugKey) =>
    flashKey.value === key ? COLOR.onPrimaryContainer : COLOR.onSurface;

  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          按键指令
        </Text>
        <View class={STYLE.actionCard}>
          <View class="w-full flex-row gap-2">
            <View focusable onPress={() => press('a')} class={keyBtnClass('a', 'fixed')}>
              <Text class="text-sm font-bold" style={{ textColor: keyTextColor('a') }}>
                A
              </Text>
            </View>
            <View focusable onPress={() => press('home')} class={keyBtnClass('home', 'grow')}>
              <Text class="text-sm font-bold" style={{ textColor: keyTextColor('home') }}>
                唤醒 HOME
              </Text>
            </View>
          </View>
          <View focusable onPress={() => press('lr')} class={keyBtnClass('lr', 'full')}>
            <Text class="text-sm font-bold" style={{ textColor: keyTextColor('lr') }}>
              配对 L+R
            </Text>
          </View>
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
