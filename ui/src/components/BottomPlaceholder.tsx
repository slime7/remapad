/**
 * 底部避让占位：悬浮底栏（h-64 + 上下各 8 边距）盖在所有页面之上，
 * 各页在滚动列的最后放置本组件统一垫高，替代各自手写的 pb-[80]。
 */
import { View } from '@pocketjs/framework/vue-vapor/components';

/** 避让高度 = 底栏 64 + 上下边距 8×2；页面 contentH 估算引用同一数值。 */
export const BOTTOM_PLACEHOLDER_H = 80;

export function BottomPlaceholder() {
  return <View class="w-full h-[80] shrink-0" />;
}
