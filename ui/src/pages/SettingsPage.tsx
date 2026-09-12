/**
 * 设置页：rounded-16 列表（手柄设置 / 调试）。配对、模式与系统已在底部
 * 导航，触摸上下滚动。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
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
  { key: 'debug', title: '调试', glyph: ICON.bug },
];

export function SettingsPage(props: { active: () => boolean; onGo: (tab: TabKey) => void }) {
  const contentH = () =>
    TOP_PAD + ITEMS.length * ITEM_H + ITEMS.length * ITEM_GAP + BOTTOM_PLACEHOLDER_H;
  const scroller = usePageScroll(props.active, contentH);

  return (
    <View class="w-full h-full overflow-hidden">
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
