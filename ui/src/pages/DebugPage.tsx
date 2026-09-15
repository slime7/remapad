/**
 * 调试页：分区承载各类测试工具，当前只有「按键指令」区——把单次按键
 * 注入数据面（保持后自动释放），经合成输入源叠加进 BLE 输入报文，
 * 用于实机手动验证上报链路；「配对 L+R」模拟同时按住 L 和 R，对应主机
 * Grip/顺序界面的配对确认；「手柄操控屏幕」注入操控组合键，不插手柄也能
 * 进出手柄控屏模式（进入后用其余按键名操作界面）。后续测试区在滚动列里
 * 继续向下追加。
 */
import { ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { sendDebugKey } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PAD_H } from '../components/BottomPlaceholder';
import type { DebugKey } from '../bridge/protocol';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';

/** 注入确认高亮的持续帧数：60Hz 下 9 帧约 150ms。 */
const FLASH_TICKS = 9;
/** 按键指令卡高度：py-3 上下 24 + 三行按钮（后两行各 mt-2 8 + 44）。 */
const KEYS_CARD_H = 24 + 8 + 44 + 8 + 44 + 8 + 44;
/** 三行按钮在内容坐标里的位置：说明文本 15 + 间距 8，卡内 py-3 再留 12。 */
const CARD_TOP = 34 + 15 + 8;
const FIRST_ROW_Y = CARD_TOP + 12;
const ROW_STEP = 44 + 8;

export function DebugPage(props: {
  active: () => boolean;
  /** 页面在画面上且没有弹窗盖住时才为真：焦点遍历只看这个（见 App.tsx）。 */
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

  /* 可聚焦按钮的位置：卡片里三行，后两行各带 mt-2。 */
  const rowNodes: Array<NodeMirror | null> = [];
  const focusRows = () => [
    { node: rowNodes[0] ?? null, y: FIRST_ROW_Y, h: 44 },
    { node: rowNodes[1] ?? null, y: FIRST_ROW_Y, h: 44 },
    { node: rowNodes[2] ?? null, y: FIRST_ROW_Y + ROW_STEP, h: 44 },
    { node: rowNodes[3] ?? null, y: FIRST_ROW_Y + ROW_STEP * 2, h: 44 },
  ];
  const contentRef = usePageScroll(
    props.active,
    true,
    () => 34 + 15 + 8 + KEYS_CARD_H + BOTTOM_PAD_H,
    focusRows,
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
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View nodeRef={contentRef} class="w-full flex-col px-4 pt-[34] gap-2">
        <Text class="text-xs shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
          按键指令
        </Text>
        <View class={STYLE.actionCard}>
          <View class="w-full flex-row gap-2">
            <View
              nodeRef={(node: NodeMirror | null) => (rowNodes[0] = node)}
              focusable={props.interactive()}
              onPress={() => press('a')}
              class={keyBtnClass('a', 'fixed')}
            >
              <Text class="text-sm font-bold" style={{ textColor: keyTextColor('a') }}>
                A
              </Text>
            </View>
            <View
              nodeRef={(node: NodeMirror | null) => (rowNodes[1] = node)}
              focusable={props.interactive()}
              onPress={() => press('home')}
              class={keyBtnClass('home', 'grow')}
            >
              <Text class="text-sm font-bold" style={{ textColor: keyTextColor('home') }}>
                唤醒 HOME
              </Text>
            </View>
          </View>
          <View
            nodeRef={(node: NodeMirror | null) => (rowNodes[2] = node)}
            focusable={props.interactive()}
            onPress={() => press('lr')}
            class={keyBtnClass('lr', 'full')}
          >
            <Text class="text-sm font-bold" style={{ textColor: keyTextColor('lr') }}>
              配对 L+R
            </Text>
          </View>
          {/* 手柄操控屏幕的组合键：不插手柄也能进模式，随后用 up/down/left/right
              与 a（圆圈键）注入的按键操作界面，再点一次本键退出。 */}
          <View
            nodeRef={(node: NodeMirror | null) => (rowNodes[3] = node)}
            focusable={props.interactive()}
            onPress={() => press('ui')}
            class={keyBtnClass('ui', 'full')}
          >
            <Text class="text-sm font-bold" style={{ textColor: keyTextColor('ui') }}>
              手柄操控屏幕
            </Text>
          </View>
        </View>
        <BottomPlaceholder />
      </View>
    </View>
  );
}
