/**
 * 悬浮底部菜单：状态 / 配对 / 模式 / 设置 / 系统 五个导航键，绝对定位悬浮
 * 在所有页面之上，图标为构建期光栅化的 SVG（nav-*.svg，配色变体随状态切换）。
 * 贴近屏幕角落的「状态 / 设置」取 rounded-[24]，中间三键取 rounded-[16]；
 * 配色规则与旧版一致：状态键 active 用 tertiary 家族，其余键用 primary 家族，
 * disabled 时仅前景置灰。
 * 滚动页在内容末尾放 BottomPlaceholder 垫高，避免最后一段被菜单遮挡。
 * disabled() 为真时（如配对进行中）忽略切换并置灰按钮，锁定当前页面。
 *
 * 注意 Vue Vapor 响应性：条件必须以函数调用形式出现在 JSX 里（读取发生在
 * 渲染作用内才会被跟踪），setup 期赋值的常量不会随 props 更新。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';

export type TabKey = 'home' | 'pairing' | 'mode' | 'settings' | 'controller' | 'system' | 'debug';

interface NavTab {
  key: TabKey;
  label: string;
  svg: 'home' | 'bt' | 'mode' | 'set' | 'sys';
}

/** 顺序即布局：两端为角落键（rounded-24），中间三键（rounded-16）。 */
const TABS: NavTab[] = [
  { key: 'home', label: '状态', svg: 'home' },
  { key: 'pairing', label: '配对', svg: 'bt' },
  { key: 'mode', label: '模式', svg: 'mode' },
  { key: 'settings', label: '设置', svg: 'set' },
  { key: 'system', label: '系统', svg: 'sys' },
];

/** 图标资源名必须是完整字面量（构建期按字面量扫描打包），不接受拼接。 */
const NAV_SRC: Record<NavTab['svg'], { on: string; off: string; dis: string }> = {
  home: { on: 'nav-home-on.svg', off: 'nav-home-off.svg', dis: 'nav-home-dis.svg' },
  bt: { on: 'nav-bt-on.svg', off: 'nav-bt-off.svg', dis: 'nav-bt-dis.svg' },
  mode: { on: 'nav-mode-on.svg', off: 'nav-mode-off.svg', dis: 'nav-mode-dis.svg' },
  set: { on: 'nav-set-on.svg', off: 'nav-set-off.svg', dis: 'nav-set-dis.svg' },
  sys: { on: 'nav-sys-on.svg', off: 'nav-sys-off.svg', dis: 'nav-sys-dis.svg' },
};

/** 各键 active/背景家族：状态键 pink 家族，其余 blue 家族。 */
const PINK_TABS: TabKey[] = ['home'];

export function AppNavBar(props: {
  tab: TabKey;
  disabled: () => boolean;
  onChange: (tab: TabKey) => void;
}) {
  const pick = (next: TabKey) => {
    if (!props.disabled()) {
      props.onChange(next);
    }
  };
  return (
    <View class="absolute left-4 right-4 bottom-4 h-[64] flex-row gap-2 z-40">
      {TABS.map((item) => {
        const active = () => props.tab === item.key;
        const pink = PINK_TABS.includes(item.key);
        const corner = item.key === 'home' || item.key === 'settings';
        const bg = () => {
          if (!props.disabled()) {
            if (active()) {
              return pink ? STYLE.navHomeOn : corner ? STYLE.navCornerOn : STYLE.navMidOn;
            }
          }
          return pink ? STYLE.navHomeOff : corner ? STYLE.navCornerOff : STYLE.navMidOff;
        };
        const iconSrc = () =>
          props.disabled()
            ? NAV_SRC[item.svg].dis
            : active()
              ? NAV_SRC[item.svg].on
              : NAV_SRC[item.svg].off;
        const labelColor = () =>
          props.disabled()
            ? COLOR.disabled
            : active()
              ? pink
                ? COLOR.onTertiaryContainer
                : COLOR.onPrimaryContainer
              : pink
                ? COLOR.onSecondaryContainer
                : COLOR.onSurfaceVariant;
        return (
          <View
            key={item.key}
            focusable
            onPress={() => pick(item.key)}
            class={bg()}
          >
            <Image class="w-[20] h-[20] shrink-0" src={iconSrc()} />
            <Text class="text-xs shrink-0" style={{ textColor: labelColor() }}>
              {item.label}
            </Text>
          </View>
        );
      })}
    </View>
  );
}
