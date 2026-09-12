/**
 * 悬浮底部菜单：状态 + 设置两个大按钮，绝对定位悬浮在所有页面之上。
 * 滚动页在内容末尾放 BottomPlaceholder 垫高，避免最后一段被菜单遮挡。
 * disabled() 为真时（如配对进行中）忽略切换并置灰按钮，锁定当前页面。
 *
 * 注意 Vue Vapor 响应性：条件必须以函数调用形式出现在 JSX 里（读取发生在
 * 渲染作用内才会被跟踪），setup 期赋值的常量不会随 props 更新。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { COLOR } from '../theme';

export type TabKey = 'home' | 'settings' | 'pairing' | 'mode' | 'system' | 'debug';

/** 禁用态的置灰颜色。 */
const DISABLED_TINT = '#5b6a85';

export function AppNavBar(props: {
  tab: TabKey;
  disabled: () => boolean;
  onChange: (tab: TabKey) => void;
}) {
  const statusActive = () => props.tab === 'home';
  const settingsActive = () => props.tab === 'settings';
  const pick = (next: TabKey) => {
    if (!props.disabled()) {
      props.onChange(next);
    }
  };
  return (
    <View class="absolute left-4 right-4 bottom-4 h-[64] flex-row gap-2 z-40">
      <View
        focusable
        onPress={() => pick('home')}
        class={
          statusActive()
            ? 'grow h-[64] rounded-[24] bg-[#fd9ace] flex-col items-center justify-center active:bg-[#ffafd7] transition-colors duration-150'
            : 'grow h-[64] rounded-[24] bg-[#34202c] flex-col items-center justify-center active:bg-[#4a2f40] transition-colors duration-150'
        }
      >
        <Icon
          glyph={ICON.home}
          class="shrink-0 text-xl"
          color={props.disabled() ? DISABLED_TINT : statusActive() ? '#631847' : '#b99cab'}
        />
        <Text
          class={
            props.disabled()
              ? 'text-xs text-[#5b6a85]'
              : statusActive()
                ? 'text-xs font-bold text-[#631847]'
                : 'text-xs text-[#b99cab]'
          }
        >
          状态
        </Text>
      </View>
      <View
        focusable
        onPress={() => pick('settings')}
        class={
          settingsActive()
            ? 'w-[64] h-[64] shrink-0 rounded-[24] bg-[#9ecefe] flex-col items-center justify-center active:bg-[#b8dbff] transition-colors duration-150'
            : 'w-[64] h-[64] shrink-0 rounded-[24] bg-[#14263e] flex-col items-center justify-center active:bg-[#1c3350] transition-colors duration-150'
        }
      >
        <Icon
          glyph={ICON.settings}
          class="shrink-0 text-xl"
          color={props.disabled() ? DISABLED_TINT : settingsActive() ? '#04456e' : '#9aacca'}
        />
        <Text
          class={
            props.disabled()
              ? 'text-xs text-[#5b6a85]'
              : settingsActive()
                ? 'text-xs font-bold text-[#04456e]'
                : 'text-xs text-[#9aacca]'
          }
        >
          设置
        </Text>
      </View>
    </View>
  );
}
