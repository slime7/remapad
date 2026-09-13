/**
 * 悬浮底部菜单：状态（宽键）+ 设置（方键），绝对定位悬浮在所有页面之上，
 * 导航距屏幕左右与底部各 8 px，按钮间距 8 px。
 * 按钮背景是构建期光栅化的 SVG（nav-*.svg）：PocketJS 原生表面只有单一
 * radius 字段，做不了「贴屏幕角的一角 32、其余三角 8」的单角圆角，因此
 * 按钮形状整体画进 SVG。圆角曲线必须用三次贝塞尔（C）表达——官方烘焙器
 * 对 path 圆弧指令（A）光栅化错位，与分辨率无关。
 * 贴图按 2 的幂且单边不超过 64 拆分（预览 wasm 核心对超过 64 的一边渲染
 * 异常）：宽键（grow 后 152×64）由左半 64×64 + 中段 32×64 + 右半 64×64
 * 拼接，相邻两块同色重叠 8 px、接缝落在实心区；方键 64×64 单图。图标与
 * 文字放在绝对定位覆盖层内居中，配色行为
 * 与旧版一致：状态键 tertiary 家族、设置键 primary 家族，disabled 时仅
 * 前景置灰、背景保持当前 on/off 形态。
 * 滚动页在滚动列末尾放 BottomPlaceholder 垫高，避免最后一段被菜单遮挡。
 * disabled() 为真时（如配对进行中）忽略切换并置灰按钮，锁定当前页面。
 *
 * 注意 Vue Vapor 响应性：条件必须以函数调用形式出现在 JSX 里（读取发生在
 * 渲染作用内才会被跟踪），setup 期赋值的常量不会随 props 更新。
 */
import { Image, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { COLOR } from '../theme';

export type TabKey =
  | 'home'
  | 'settings'
  | 'controller'
  | 'pairing'
  | 'mode'
  | 'system'
  | 'debug';

/** 背景资源名必须是完整字面量（构建期按字面量扫描打包），不接受拼接。 */
const HOME_BG = {
  onL: 'nav-home-on-l.svg',
  onMid: 'nav-home-on-mid.svg',
  onR: 'nav-home-on-r.svg',
  offL: 'nav-home-off-l.svg',
  offMid: 'nav-home-off-mid.svg',
  offR: 'nav-home-off-r.svg',
};
const SET_BG = { on: 'nav-set-on.svg', off: 'nav-set-off.svg' };

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
    <View class="absolute left-[8] right-[8] bottom-[8] h-[64] flex-row gap-2 z-40">
      <View focusable onPress={() => pick('home')} class="grow h-[64] flex-col items-center justify-center">
        <Image
          class="absolute left-0 top-0 w-[64] h-[64]"
          src={statusActive() ? HOME_BG.onL : HOME_BG.offL}
        />
        <Image
          class="absolute left-[64] top-0 w-[32] h-[64]"
          src={statusActive() ? HOME_BG.onMid : HOME_BG.offMid}
        />
        <Image
          class="absolute left-[88] top-0 w-[64] h-[64]"
          src={statusActive() ? HOME_BG.onR : HOME_BG.offR}
        />
        <Icon
          glyph={ICON.home}
          class="shrink-0 text-xl"
          color={props.disabled() ? COLOR.disabled : statusActive() ? COLOR.onTertiaryContainer : COLOR.onSecondaryContainer}
        />
        <Text
          class={props.disabled() || !statusActive() ? 'text-xs' : 'text-xs font-bold'}
          style={{
            textColor: props.disabled()
              ? COLOR.disabled
              : statusActive()
                ? COLOR.onTertiaryContainer
                : COLOR.onSecondaryContainer,
          }}
        >
          状态
        </Text>
      </View>
      <View focusable onPress={() => pick('settings')} class="w-[64] h-[64] shrink-0 flex-col items-center justify-center">
        <Image
          class="absolute left-0 top-0 w-[64] h-[64]"
          src={settingsActive() ? SET_BG.on : SET_BG.off}
        />
        <Icon
          glyph={ICON.settings}
          class="shrink-0 text-xl"
          color={props.disabled() ? COLOR.disabled : settingsActive() ? COLOR.onPrimaryContainer : COLOR.onSurfaceVariant}
        />
        <Text
          class={props.disabled() || !settingsActive() ? 'text-xs' : 'text-xs font-bold'}
          style={{
            textColor: props.disabled()
              ? COLOR.disabled
              : settingsActive()
                ? COLOR.onPrimaryContainer
                : COLOR.onSurfaceVariant,
          }}
        >
          设置
        </Text>
      </View>
    </View>
  );
}
