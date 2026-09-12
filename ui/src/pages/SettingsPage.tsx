/**
 * 设置页：rounded-16 列表（配对 / 模式 / 系统），触摸上下滚动。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR } from '../theme';
import type { TabKey } from '../components/AppNavBar';

const ITEM_H = 44;
const ITEM_GAP = 8;
/** 顶部留白：26px 状态栏覆盖层 + 8px 间距。 */
const TOP_PAD = 34;
/** 底部预留：96px 底栏避让 + 24px 滚动余量，保证列表可滚，随 item 数自动增长。 */
const BOTTOM_RESERVE = 120;

interface SettingsItem {
  key: TabKey;
  title: string;
  glyph: string;
}

const ITEMS: SettingsItem[] = [
  { key: 'pairing', title: '手柄配对', glyph: ICON.bluetooth },
  { key: 'mode', title: '模式切换', glyph: ICON.swapHoriz },
  { key: 'system', title: '系统', glyph: ICON.settings },
];

export function SettingsPage(props: { active: () => boolean; onGo: (tab: TabKey) => void }) {
  const contentH = () =>
    TOP_PAD + ITEMS.length * ITEM_H + (ITEMS.length - 1) * ITEM_GAP + BOTTOM_RESERVE;
  const scroller = usePageScroll(props.active, contentH);

  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] pb-[96] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        {ITEMS.map((item) => (
          <View
            key={item.key}
            focusable
            onPress={() => props.onGo(item.key)}
            class="w-full h-[44] shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 active:bg-[#14263e] transition-colors duration-150"
          >
            <Icon glyph={item.glyph} class="shrink-0 text-lg" color={COLOR.primary} />
            <Text class="text-sm text-[#d9e6ff] ml-3 grow">{item.title}</Text>
            <Icon glyph={ICON.chevronRight} class="shrink-0 text-lg" color={COLOR.outline} />
          </View>
        ))}
      </View>
    </View>
  );
}
