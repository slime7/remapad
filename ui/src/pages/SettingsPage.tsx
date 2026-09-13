/**
 * 设置页：rounded-16 列表菜单（手柄设置 / 配对 / 模式 / 系统 / 调试）。
 * 底部导航只有状态与设置两个键，其余功能页都从这里进入，触摸上下滚动。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder, BOTTOM_PAD_H } from '../components/BottomPlaceholder';
import type { TabKey } from '../components/AppNavBar';

const ITEM_H = 44;
const ITEM_GAP = 8;
/** 顶部留白：26px 状态栏覆盖层 + 8px 间距。 */
const TOP_PAD = 34;

interface SettingsItem {
  key: TabKey;
  title: string;
  glyph: string;
}

const ITEMS: SettingsItem[] = [
  { key: 'controller', title: '手柄设置', glyph: ICON.gamepad },
  { key: 'pairing', title: '手柄配对', glyph: ICON.bluetooth },
  { key: 'mode', title: '模式切换', glyph: ICON.swapHoriz },
  { key: 'system', title: '系统', glyph: ICON.settings },
  { key: 'debug', title: '调试', glyph: ICON.bug },
];

export function SettingsPage(props: { active: () => boolean; onGo: (tab: TabKey) => void }) {
  const contentH = () =>
    TOP_PAD + ITEMS.length * ITEM_H + (ITEMS.length - 1) * ITEM_GAP + BOTTOM_PAD_H;
  const scroller = usePageScroll(props.active, true, contentH);
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        {ITEMS.map((item) => (
          <View
            key={item.key}
            focusable
            onPress={() => props.onGo(item.key)}
            class={STYLE.rowCard}
          >
            <Icon glyph={item.glyph} class="shrink-0 text-lg" color={COLOR.primary} />
            <Text class="text-sm ml-3 grow" style={{ textColor: COLOR.onSurface }}>
              {item.title}
            </Text>
            <Icon glyph={ICON.chevronRight} class="shrink-0 text-lg" color={COLOR.outline} />
          </View>
        ))}
        <BottomPlaceholder />
      </View>
    </View>
  );
}
