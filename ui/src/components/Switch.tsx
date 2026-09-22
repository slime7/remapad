/**
 * MD3 风格开关（设置页复用的独立组件）：
 * 可点区域是 40 × 40 的命中区（focusable 与 onPress 都在它身上），36 × 20 的
 * 轨道与 14 的滑块画在命中区里居中——触摸目标够大而开关视觉尺寸不变。
 * 滑块位置由轨道的 justify-start / justify-end 决定，因此不需要定位容器。
 * 选中态只换整条 class 字面量（轨道底色与对齐、滑块底色），不做透明度混合
 * ——S3 软件渲染器的逐像素混合是最大的单帧开销。
 */
import { View } from '@pocketjs/framework/vue-vapor/components';
import { STYLE } from '../theme';

export function Switch(props: {
  /** 当前状态（响应式读取，写入即翻）。 */
  on: () => boolean;
  /** 点按与圆圈键确认时的翻转动作。 */
  onToggle: () => void;
  /** 焦点环准入：App 传下来的 interactive()，隐藏页的控件不留在焦点名单里。 */
  interactive: () => boolean;
}) {
  return (
    <View
      focusable={props.interactive()}
      onPress={props.onToggle}
      class={props.on() ? STYLE.switchHitOn : STYLE.switchHitOff}
    >
      <View class={props.on() ? STYLE.switchTrackOn : STYLE.switchTrackOff}>
        <View class={props.on() ? STYLE.switchThumbOn : STYLE.switchThumbOff} />
      </View>
    </View>
  );
}
