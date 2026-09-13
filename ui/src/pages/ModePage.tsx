/** 模式页：USB 两种角色——串口调试、手柄（实体手柄转发）。角色只对本次运行
 *  生效，重启回到串口（固件不写 NVS）。桥接（otg）开发期临时禁用防误操作：USB PHY 切换会断开 COM 串口，
 *  数据面也未接入，UI 直接不展示该选项，后端对 otg 请求同样静默跳过，
 *  待 M5 数据面接入后恢复三选项。运行时 Text 是单行图元（官方契约
 *  "one inline run"），多行描述按行拆成多个 Text 节点；卡片不固定高度，
 *  用 py-3 留白，图标随内容垂直居中。 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { Icon, ICON } from '../icons';
import { usePageScroll } from '../hooks/usePageScroll';
import { useMountCursor } from '../hooks/useProgressiveMount';
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
    () => 34 + CARD_H * 2 + 8 * 2 + 20 * 2 + BOTTOM_PLACEHOLDER_H,
  );
  /** 分帧填充：两张角色卡、两行说明、底部占位。 */
  const step = useMountCursor(5);
  return (
    <View class={props.active() ? 'w-full h-full overflow-hidden' : 'hidden'}>
      <View
        class="w-full flex-col px-4 pt-[34] gap-2"
        style={{ translateY: -scroller.offset() }}
      >
        {step() >= 1 ? (
          <RoleCard
            role="device"
            selected={hw.usbRole === 'device'}
            title="串口"
            lines={['烧录 / 日志']}
            glyph={ICON.adb}
            onSelect={() => setUsbRole('device')}
          />
        ) : null}
        {step() >= 2 ? (
          <RoleCard
            role="host"
            selected={hw.usbRole === 'host'}
            title="手柄"
            lines={['手柄输入 → NS2']}
            glyph={ICON.gamepad}
            onSelect={() => setUsbRole('host')}
          />
        ) : null}
        {step() >= 3 && hw.roleMessage ? (
          <Text class="text-xs text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
            {hw.roleMessage}
          </Text>
        ) : null}
        {step() >= 4 ? (
          <Text class="text-xs text-center shrink-0" style={{ textColor: COLOR.onSurfaceVariant }}>
            重启后回到串口
          </Text>
        ) : null}
        {step() >= 5 ? <BottomPlaceholder /> : null}
      </View>
    </View>
  );
}
