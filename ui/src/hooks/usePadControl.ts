/**
 * 手柄操控 UI：键盘与手柄按键驱动的焦点窗口。
 *
 * 焦点清单交给框架：每个页面把自己的 focusable 绑在「当前可见、且没有弹窗
 * 盖住」上（见各页的 interactive 属性），所以框架的清单天然只有当前画面上
 * 的控件——七个页面在首帧就全挂在树上、切页只翻 hidden（见 ADR 0016），
 * 静态的 focusable 会把隐藏页的控件也留在名单里，手柄就会「屏幕上看不到
 * 焦点、圆圈键却按得到」。
 *
 * 方向分工（见 ADR 0028）：上下在页面内容里走，焦点已经在最后一项再按下就让
 * 页面继续往下滚；左右只在悬浮底栏两项之间走，真机上的 L1 / R1 与左右等价。
 * 两个轴各自绑在一棵子树上——页面容器与底栏各注册一个焦点控制器，框架按
 * 「焦点在谁里面」挑一个调用，于是左右不会掉进页面内容、上下也不会停在底栏。
 *
 * 这里还负责两件事：
 *   - 记住最近一次手柄按键，窗口内保留焦点环（实机上整个模式期间由固件维持）；
 *   - 不在操控状态时清掉焦点：触摸点按也会设置焦点（框架的 touch-activation
 *     走 pressNode），不清掉屏幕上会一直挂着一个环。
 */
import { onScopeDispose } from 'vue';
import { focusNode, getFocused, pushFocusController } from '@pocketjs/framework/vue-vapor/input';
import type { FocusDirection } from '@pocketjs/framework/vue-vapor/input';
import { onButtonPress, onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { virtualNow } from '@pocketjs/framework/vue-vapor/clock';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';
import { padScrollPage } from './usePageScroll';

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
  /** 页面容器节点：上下键只在这棵子树里走。 */
  pageRoot: () => NodeMirror | null;
  /** 悬浮底栏节点：左右键只在这棵子树里走。 */
  navRoot: () => NodeMirror | null;
}

export interface PadControl {
  /** 当前是否处于手柄操控（固件模式，或最近有手柄按键进来）。 */
  active: () => boolean;
}

export function usePadControl(options: PadControlOptions): PadControl {
  /** 最近一次手柄按键的虚拟时间（秒）；0 表示从未按下。 */
  let lastKeyAt = 0;
  /** 最后停在页面内容里的节点：从底栏回到内容时优先回它。 */
  let lastPageNode: NodeMirror | null = null;
  let stopPage: (() => void) | null = null;
  let stopNav: (() => void) | null = null;

  const active = (): boolean =>
    options.firmwareMode() || (lastKeyAt !== 0 && virtualNow() - lastKeyAt <= PAD_IDLE_SEC);

  /** 子树里的可聚焦节点：框架的清单只看 focusable 标记，隐藏页与弹窗背后的
   *  控件都不是可聚焦的，所以这里只拿得到画面上的控件。 */
  const focusablesWithin = (root: NodeMirror | null): NodeMirror[] => {
    const out: NodeMirror[] = [];
    const collect = (node: NodeMirror | null): void => {
      if (node === null) {
        return;
      }
      if (node.focusable === true) {
        out.push(node);
      }
      const children = node.children;
      if (!Array.isArray(children)) {
        return;
      }
      for (let index = 0; index < children.length; index += 1) {
        collect(children[index]);
      }
    };
    collect(root);
    return out;
  };

  const focusPageNode = (node: NodeMirror): void => {
    lastPageNode = node;
    focusNode(node);
  };

  /**
   * 两个轴各在自己的子树里走。返回 false 交给框架的默认遍历：模式里第一次
   * 按键（此刻还没有焦点，框架从默认清单的对应端进入）与焦点在弹窗上（弹窗
   * 不在页面容器与底栏这两棵子树里）。
   */
  const move = (direction: FocusDirection): boolean => {
    const nav = focusablesWithin(options.navRoot());
    const page = focusablesWithin(options.pageRoot());
    const focused = getFocused();
    if (focused === null) {
      return false;
    }
    const navIndex = nav.indexOf(focused);
    const pageIndex = page.indexOf(focused);
    if (navIndex < 0 && pageIndex < 0) {
      return false;
    }

    if (direction === 'left' || direction === 'right') {
      if (nav.length === 0) {
        return true;
      }
      if (pageIndex >= 0) {
        lastPageNode = focused; // 离开内容之前记下位置
      }
      const back = direction === 'left' ? -1 : 1;
      const next = navIndex < 0 ? (back < 0 ? 0 : nav.length - 1) : navIndex + back;
      if (next !== navIndex && next >= 0 && next < nav.length) {
        focusNode(nav[next]);
      }
      return true;
    }

    if (navIndex >= 0) {
      // 环在底栏：上下回到页面内容，触摸点过底栏之后不会卡在栏上。
      if (page.length === 0) {
        return true;
      }
      const remembered = lastPageNode;
      focusPageNode(
        remembered !== null && page.indexOf(remembered) >= 0 ? remembered : page[page.length - 1],
      );
      return true;
    }

    const next = pageIndex + (direction === 'down' ? 1 : -1);
    if (next >= 0 && next < page.length) {
      focusPageNode(page[next]);
      return true;
    }
    // 已经在内容的最后一项：再按下时页面自己继续往下滚（滚到页底为止）。
    if (direction === 'down') {
      padScrollPage();
    }
    return true;
  };

  onButtonPress(PAD_KEYS, () => {
    lastKeyAt = virtualNow();
  });

  onFrame(() => {
    /* 两棵子树要等挂载时才拿到节点，这里各补一次注册。 */
    const pageRoot = options.pageRoot();
    if (stopPage === null && pageRoot !== null) {
      stopPage = pushFocusController(pageRoot, move);
    }
    const navRoot = options.navRoot();
    if (stopNav === null && navRoot !== null) {
      stopNav = pushFocusController(navRoot, move);
    }
    if (!active() && getFocused() !== null) {
      focusNode(null);
    }
    if (getFocused() === null) {
      lastPageNode = null;
    }
  });

  onScopeDispose(() => {
    stopPage?.();
    stopNav?.();
    if (getFocused() !== null) {
      focusNode(null);
    }
  });

  return { active };
}
