/**
 * 手柄操控 UI：键盘与手柄按键驱动的焦点与导航控制器。
 *
 * 契约：
 * - 左右方向键（以及实体手柄的 L1/R1）：切换上方四叶草页面（无限循环切页）；
 * - 上下方向键：在当前激活页面的交互控件之间上下移动焦点；
 * - 圆圈键（Enter / 空格）：确认触发当前聚焦控件；
 * - 保持与 ADR 0028 / ADR 0029 解耦分工一致。
 */
import { onScopeDispose } from 'vue';
import { focusNode, getFocused, pushFocusController } from '@pocketjs/framework/vue-vapor/input';
import type { FocusDirection } from '@pocketjs/framework/vue-vapor/input';
import { onButtonPress, onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { virtualNow } from '@pocketjs/framework/vue-vapor/clock';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';

/** 手柄按键触发后的操控窗口（虚拟秒）：这段时间内焦点环可见、方向键可用。 */
const PAD_IDLE_SEC = 4;

/** 按键位掩码定义（对齐 PocketJS BTN 规范）。 */
const BTN_UP = 0x0010;
const BTN_RIGHT = 0x0020;
const BTN_DOWN = 0x0040;
const BTN_LEFT = 0x0080;
const BTN_L1 = 0x0100;
const BTN_R1 = 0x0200;
const BTN_CIRCLE = 0x2000;
const BTN_CROSS = 0x4000;

const PAD_KEYS = BTN_UP | BTN_RIGHT | BTN_DOWN | BTN_LEFT | BTN_L1 | BTN_R1 | BTN_CIRCLE | BTN_CROSS;

export interface PadControlOptions {
  /** 固件报告的「手柄操控 UI」模式。 */
  firmwareMode: () => boolean;
  /** 页面容器节点：包含上方所有页面的根节点。 */
  pageRoot: () => NodeMirror | null;
  /** 切换到上一页回调。 */
  onPrevPage: () => void;
  /** 切换到下一页回调。 */
  onNextPage: () => void;
  /** 退出手柄操控屏幕模式回调。 */
  onExitPadMode?: () => void;
}

export interface PadControl {
  /** 当前是否处于手柄操控（固件模式，或最近有手柄按键进来）。 */
  active: () => boolean;
}

export function usePadControl(options: PadControlOptions): PadControl {
  let lastKeyAt = 0;
  let stopController: (() => void) | null = null;
  let lastPageNode: NodeMirror | null = null;
  let focusNextPageFirst = false;

  const active = (): boolean =>
    options.firmwareMode() || (lastKeyAt !== 0 && virtualNow() - lastKeyAt <= PAD_IDLE_SEC);

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

  const focusFirstItem = (): void => {
    const items = focusablesWithin(options.pageRoot());
    if (items.length > 0) {
      focusPageNode(items[0]);
    }
  };

  const moveWithinPage = (delta: number): void => {
    const items = focusablesWithin(options.pageRoot());
    if (items.length === 0) {
      return;
    }

    const focused = getFocused();
    const curIndex = focused !== null ? items.indexOf(focused) : -1;
    let nextIndex = 0;
    if (curIndex < 0) {
      nextIndex = delta > 0 ? 0 : items.length - 1;
    } else {
      nextIndex = curIndex + delta;
      if (nextIndex < 0) {
        nextIndex = items.length - 1;
      }
      if (nextIndex >= items.length) {
        nextIndex = 0;
      }
    }

    focusPageNode(items[nextIndex]);
  };

  // L1 / R1 与左右键切页：统一由 onButtonPress 处理单次边沿，保证未聚焦时也能即时切页
  onButtonPress(PAD_KEYS, (mask) => {
    lastKeyAt = virtualNow();

    if ((mask & (BTN_RIGHT | BTN_R1)) !== 0) {
      options.onNextPage();
      focusNextPageFirst = true;
    } else if ((mask & (BTN_LEFT | BTN_L1)) !== 0) {
      options.onPrevPage();
      focusNextPageFirst = true;
    }
  });

  // 焦点控制器：接管方向键，左右键拦截（已被 onButtonPress 处理），上下键在页内选择
  const move = (direction: FocusDirection): boolean => {
    if (direction === 'left' || direction === 'right') {
      return true;
    }
    if (direction === 'down') {
      moveWithinPage(1);
      return true;
    }
    if (direction === 'up') {
      moveWithinPage(-1);
      return true;
    }
    return false;
  };

  onFrame(() => {
    const pageRoot = options.pageRoot();
    if (stopController === null && pageRoot !== null) {
      stopController = pushFocusController(pageRoot, move);
    }

    // 切页后在下一帧聚焦新可见页的首个控件
    if (focusNextPageFirst) {
      focusNextPageFirst = false;
      focusFirstItem();
    }

    // 非操控窗口时清除多余焦点环
    if (!active() && getFocused() !== null) {
      focusNode(null);
      lastPageNode = null;
    }
  });

  onScopeDispose(() => {
    stopController?.();
  });

  return { active };
}
