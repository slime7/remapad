/**
 * 手柄操控 UI：键盘与手柄按键驱动的焦点窗口。
 *
 * 遍历本身交给框架：每个页面把自己的 focusable 绑在「当前可见、且没有弹窗
 * 盖住」上（见各页的 interactive 属性），所以框架的清单天然只有当前画面上
 * 的控件——七个页面在首帧就全挂在树上、切页只翻 hidden（见 ADR 0016），
 * 静态的 focusable 会把隐藏页的控件也留在名单里，手柄就会「屏幕上看不到
 * 焦点、圆圈键却按得到」。
 *
 * 这里只做两件事：
 *   - 记住最近一次手柄按键，窗口内保留焦点环（实机上整个模式期间由固件维持）；
 *   - 不在操控状态时清掉焦点：触摸点按也会设置焦点（框架的 touch-activation
 *     走 pressNode），不清掉屏幕上会一直挂着一个环。
 */
import { onScopeDispose } from 'vue';
import { focusNode, getFocused } from '@pocketjs/framework/vue-vapor/input';
import { onButtonPress, onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { virtualNow } from '@pocketjs/framework/vue-vapor/clock';

/** 手柄按键触发后的操控窗口（虚拟秒）：这段时间内焦点环可见、方向键可用。
 *  实机上进入模式后由固件一直维持；浏览器预览里没有模式，靠按键本身进入。 */
const PAD_IDLE_SEC = 4;

/** 参与操控的按键：方向键与圆圈键（其余按键不参与）。 */
const DIRECTION_KEYS = 0x0010 | 0x0020 | 0x0040 | 0x0080;
const CIRCLE = 0x2000;
const PAD_KEYS = DIRECTION_KEYS | CIRCLE;

export interface PadControlOptions {
  /** 固件报告的「手柄操控 UI」模式（浏览器预览里恒为 false）。 */
  firmwareMode: () => boolean;
}

export interface PadControl {
  /** 当前是否处于手柄操控（固件模式，或最近有手柄按键进来）。 */
  active: () => boolean;
}

export function usePadControl(options: PadControlOptions): PadControl {
  /** 最近一次手柄按键的虚拟时间（秒）；0 表示从未按下。 */
  let lastKeyAt = 0;

  const active = (): boolean =>
    options.firmwareMode() || (lastKeyAt !== 0 && virtualNow() - lastKeyAt <= PAD_IDLE_SEC);

  onButtonPress(PAD_KEYS, () => {
    lastKeyAt = virtualNow();
  });

  onFrame(() => {
    if (!active() && getFocused() !== null) {
      focusNode(null);
    }
  });

  onScopeDispose(() => {
    if (getFocused() !== null) {
      focusNode(null);
    }
  });

  return { active };
}
