/**
 * 页面底部垫高：悬浮底栏（h-64 + 上下各 8 边距）盖在所有页面之上，
 * 滚动页的滚动列末尾放本组件统一垫高；页面各自的 contentH 估算引用同一数值，
 * 少了它滚到底时最后一段会被底栏压住。
 *
 * 高度在底栏遮挡范围（64 + 8 = 72）之外再留一段兜底：页面内容高度是静态
 * 估算值，卡片内边距、行数与条件行都可能带来偏差，这段余量保证滚到底时
 * 最后一张卡片仍完整露在底栏上方。
 */
import { View } from '@pocketjs/framework/vue-vapor/components';

/** 垫高高度 = 底栏遮挡 72 + 兜底 48；页面 contentH 估算引用同一数值。 */
export const BOTTOM_PAD_H = 120;

export function BottomPlaceholder() {
  return <View class="w-full h-[120] shrink-0" />;
}
