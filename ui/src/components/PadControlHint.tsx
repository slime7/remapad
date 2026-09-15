/**
 * 手柄操控模式的提示条：组合键把输入收给屏幕后，浮在底栏上方几秒，说明
 * 上下在移动焦点、左右在切底栏、圆圈键等于点按、怎么退出。提示只挂一次节点（切换模式
 * 时翻 hidden，见 docs/ABSTRACTIONS.md 的页面组织），运行期不建树。
 *
 * 提示只在实机上出现：固件经 bridge 广播 padUiModeChanged，浏览器预览里
 * 没有这个模式（键盘随时可用，说明写在预览页的面板里）。
 */
import { Text, View } from '@pocketjs/framework/vue-vapor/components';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { ref, watch } from 'vue';
import { COLOR, STYLE } from '../theme';

/** 提示停留时长：60Hz 下 210 帧约 3.5 秒。 */
const HINT_FRAMES = 210;

export function PadControlHint(props: { active: () => boolean }) {
  const shown = ref(false);
  let remaining = 0;

  /* props.active 是函数：在 watch 里读一次，模式一变就重新计时。 */
  watch(props.active, (on) => {
    remaining = on ? HINT_FRAMES : 0;
    shown.value = on;
  });

  onFrame(() => {
    if (remaining <= 0) {
      return;
    }
    remaining -= 1;
    if (remaining === 0) {
      shown.value = false;
    }
  });

  return (
    <View class={shown.value ? STYLE.padHint : 'hidden'}>
      <Text class="text-xs font-bold" style={{ textColor: COLOR.onBackground }}>
        手柄操控中
      </Text>
      <Text class="text-xs" style={{ textColor: COLOR.onSurfaceVariant }}>
        上下移动焦点，○ 确认
      </Text>
      <Text class="text-xs" style={{ textColor: COLOR.onSurfaceVariant }}>
        左右 / L1 R1 切底栏
      </Text>
      <Text class="text-xs" style={{ textColor: COLOR.onSurfaceVariant }}>
        再按组合键退出
      </Text>
    </View>
  );
}
