/**
 * 确认对话框：本应用自绘的遮罩弹窗。官方 Modal 的 portal 层按 480×272
 * fallback 视口定位，在 240×280 上会错位，因此重启与关机共用这一形态，
 * 只有标题、两行说明与确认按钮不同。
 *
 * FocusScope 把十字键遍历与圆圈键按下限制在弹窗子树内（挂载即生效）：
 * 否则弹窗外的控件（底栏连接按钮等）仍在焦点名单里，方向键会把焦点
 * 送出弹窗、回车误触别的动作。autoFocus 让焦点落在第一颗按钮（取消），
 * 关闭后 restoreFocus 交还焦点。FocusScope 自身就是遮罩层——嵌一层
 * 无尺寸的包裹 View 会让 scrim 的 absolute inset-0 错锚到它身上。
 */
import { FocusScope, Text, View } from '@pocketjs/framework/vue-vapor/components';
import { COLOR, STYLE } from '../theme';

export function ConfirmDialog(props: {
  title: string;
  /** 两行说明文本（单行图元不换行，长文案按行拆开）。 */
  lines: [string, string];
  confirmLabel: string;
  onCancel: () => void;
  onConfirm: () => void;
}) {
  return (
    <FocusScope class={STYLE.scrim}>
      <View class={STYLE.modalBox}>
        <Text class="text-base font-bold" style={{ textColor: COLOR.onSurface }}>
          {props.title}
        </Text>
        <Text class="text-xs text-center mt-1" style={{ textColor: COLOR.onSurfaceVariant }}>
          {props.lines[0]}
        </Text>
        <Text class="text-xs text-center" style={{ textColor: COLOR.onSurfaceVariant }}>
          {props.lines[1]}
        </Text>
        <View class="flex-row gap-2 mt-3">
          <View focusable onPress={props.onCancel} class={STYLE.modalCancelBtn}>
            <Text class="text-sm" style={{ textColor: COLOR.onSurface }}>
              取消
            </Text>
          </View>
          <View focusable onPress={props.onConfirm} class={STYLE.modalDangerBtn}>
            <Text class="text-sm font-bold" style={{ textColor: COLOR.onErrorContainer }}>
              {props.confirmLabel}
            </Text>
          </View>
        </View>
      </View>
    </FocusScope>
  );
}
