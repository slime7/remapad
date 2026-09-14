/**
 * 确认对话框：本应用自绘的遮罩弹窗。官方 Modal 的 portal 层按 480×272
 * fallback 视口定位，在 240×280 上会错位，因此重启与关机共用这一形态，
 * 只有标题、两行说明与确认按钮不同。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
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
    <View class={STYLE.scrim}>
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
    </View>
  );
}
