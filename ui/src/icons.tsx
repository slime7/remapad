/**
 * 图标组件：按字形表把 Material Icons 画成单色文本。
 *
 * 字形表在 iconGlyphs.ts（纯数据，端到端用例在 Node 侧直接引用同一份取值）。
 * 图标走字体烘焙：MaterialIcons-Regular.ttf 在 fonts.json 里注册为回退字体面，
 * 构建期按字形表里的字面量烘焙进各字号槽位；颜色用 textColor 控制，字号只能用 Tailwind 槽位
 * （xs 12 / sm 14 / base 16 / lg 18 / xl 20 / 2xl 24）。
 */
import { Text } from '@pocketjs/framework/vue-vapor/components';
import { COLOR } from './theme';
import { ICON } from './iconGlyphs';

export { ICON };

interface IconProps {
  glyph: string;
  /** 完整 class 字面量（含字号槽位与 shrink-0），在调用处声明以便扫描烘焙。 */
  class: string;
  color?: string;
}

export function Icon(props: IconProps) {
  return (
    <Text
      class={props.class}
      style={{ textColor: props.color ?? COLOR.onSurface, translateY: 2 }}
    >
      {props.glyph}
    </Text>
  );
}
