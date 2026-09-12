/** 模式页：USB 三种角色——串口调试、桥接（PC 输入转发）、手柄（实体手柄转发）。
 *  角色选择经固件持久化；桥接（otg）双端禁切（USB PHY 切换会断开 COM，
 *  且数据面未接入），点击只给出提示。运行时 Text 是单行图元（官方契约
 *  "one inline run"），多行描述按行拆成多个 Text 节点；卡片不固定高度，
 *  用 py-3 留白，图标随内容垂直居中。 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { COLOR, STYLE } from '../theme';
import { hw, setUsbRole } from '../hooks/useHardware';
import { BottomPlaceholder, BOTTOM_PLACEHOLDER_H } from '../components/BottomPlaceholder';
import type { UsbRole } from '../bridge/protocol';

/** 卡片高度：py-3 上下 24 + 标题行 18 + 描述行 15（单行描述）。 */
const CARD_H = 24 + 18 + 15;

function RoleCard(props: {
  role: UsbRole;
  title: string;
  lines: string[];
  selected: boolean;
  onSelect: () => void;
  glyph: string;
}) {
  return (
    <View
      focusable
      onPress={props.onSelect}
      class={props.selected ? STYLE.optionCardSel : STYLE.optionCard}
    >
      <Icon
        glyph={props.glyph}
        class="shrink-0 text-2xl"
        color={props.selected ? COLOR.onPrimaryContainer : COLOR.primary}
      />
      <View class="flex-col grow">
        <Text
          class="text-sm font-bold"
          style={{ textColor: props.selected ? COLOR.onPrimaryContainer : COLOR.onSurface }}
        >
          {props.title}
        </Text>
        {props.lines.map((line) => (
          <Text
            class="text-xs"
            style={{ textColor: props.selected ? COLOR.onPrimary : COLOR.onSurfaceVariant }}
          >
            {line}
          </Text>
        ))}
      </View>
    </View>
  );
}

export function ModePage(props: { active: () => boolean }) {
  const scroller = usePageScroll(
    props.active,
    () => 34 + CARD_H * 3 + 8 * 3 + 20 + BOTTOM_PLACEHOLDER_H,
  );
  return (
    <View class="w-full h-full overflow-hidden">
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        <RoleCard
          role="device"
          selected={hw.usbRole === 'device'}
          title="串口"
          lines={['烧录 / 日志']}
          glyph={ICON.adb}
          onSelect={() => setUsbRole('device')}
        />
        <RoleCard
          role="otg"
          selected={hw.usbRole === 'otg'}
          title="桥接"
          lines={['电脑输入 → NS2', '数据面接入前暂不可切换']}
          glyph={ICON.computer}
          onSelect={() => setUsbRole('otg')}
        />
        <RoleCard
          role="host"
          selected={hw.usbRole === 'host'}
          title="手柄"
          lines={['手柄输入 → NS2']}
          glyph={ICON.gamepad}
          onSelect={() => setUsbRole('host')}
        />
        {hw.roleMessage ? (
          <Text class="text-xs text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
            {hw.roleMessage}
          </Text>
        ) : null}
        <BottomPlaceholder />
      </View>
    </View>
  );
}
