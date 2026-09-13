/**
 * 横向滚动文字：PocketJS 的 Text 是单行图元（官方契约 "one inline run"，不会
 * 自动换行），宽度放不下时用位移把整段文字展示出来。
 *
 * 节奏：静止 holdStartMs（默认 2 秒）→ 以 speedPxPerSec 匀速向左滚到文本
 * 末端 → 尽头停留 holdEndMs（默认 1 秒）→ 跳回起点循环。文本放得下时全程
 * 静止，且不写入任何属性。
 *
 * 可视区宽度只能由调用方给出：框架没有布局回读接口，文本宽度由宿主
 * measureText 按字体槽位量出，因此调用处的布局宽度必须与 width 参数一致。
 * textClass 必须是完整 class 字面量（构建期按字面量注册样式），且要在下表
 * 里有对应槽位，否则按 text-xs 处理。
 */
import { computed, ref, watch } from 'vue';
import { getOps } from '@pocketjs/framework/vue-vapor';
import { virtualNow } from '@pocketjs/framework/vue-vapor/clock';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR } from '../theme';

/** textClass 字面量 → 烘焙字体槽位（见快照的 styles.generated.ts FONT_SLOTS）。 */
const FONT_SLOT_BY_CLASS: Record<string, number> = {
  'text-xs': 0,
  'text-xs font-bold': 7,
  'text-sm': 1,
  'text-sm font-bold': 8,
  'text-base': 2,
  'text-base font-bold': 9,
};

export interface MarqueeTextProps {
  text: string;
  /** 可视区宽度（逻辑像素），与调用处的布局宽度保持一致。 */
  width: number;
  /** 完整 class 字面量，默认 text-xs。 */
  textClass?: string;
  color?: string;
  /** 匀速滚动速度，像素/秒。 */
  speedPxPerSec?: number;
  /** 起点静止时长（毫秒）。 */
  holdStartMs?: number;
  /** 滚到末端后的停留时长（毫秒）。 */
  holdEndMs?: number;
  /** 页面可见性；为假时冻结并复位到起点（默认恒为可见）。 */
  active?: () => boolean;
}

export function MarqueeText(props: MarqueeTextProps) {
  const textClass = () => props.textClass ?? 'text-xs';
  const slot = () => FONT_SLOT_BY_CLASS[textClass()] ?? 0;
  const textWidth = computed(() => {
    const ops = getOps();
    return typeof ops.measureText === 'function' ? ops.measureText(props.text, slot()) : 0;
  });
  const overflow = () => Math.max(0, textWidth.value - props.width);
  const speed = () => Math.max(1, props.speedPxPerSec ?? 40);
  const holdStart = () => props.holdStartMs ?? 2000;
  const holdEnd = () => props.holdEndMs ?? 1000;

  const offset = ref(0);
  let startedAt = virtualNow();
  const rewind = () => {
    startedAt = virtualNow();
    offset.value = 0;
  };

  // 文本、可视宽度或字体槽位变化后从起点重新计时。
  watch([() => props.text, () => props.width, () => textClass()], rewind);

  onFrame(() => {
    if (props.active !== undefined && !props.active()) {
      rewind();
      return;
    }
    const distance = overflow();
    if (distance <= 0) {
      offset.value = 0;
      return;
    }
    const scrollMs = (distance / speed()) * 1000;
    const cycle = holdStart() + scrollMs + holdEnd();
    const elapsed = ((virtualNow() - startedAt) * 1000) % cycle;
    if (elapsed < holdStart()) {
      offset.value = 0;
      return;
    }
    const travelled = Math.min(elapsed - holdStart(), scrollMs);
    offset.value = -((distance * travelled) / scrollMs);
  });

  return (
    <View class="shrink-0 overflow-hidden flex-row" style={{ width: props.width }}>
      <View class="shrink-0 flex-row" style={{ translateX: offset.value }}>
        <Text class={textClass()} style={{ textColor: props.color ?? COLOR.onSurface }}>
          {props.text}
        </Text>
      </View>
    </View>
  );
}
