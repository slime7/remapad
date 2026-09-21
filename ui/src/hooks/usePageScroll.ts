/**
 * 页面级动能滚动：全应用只注册一个纵向手势，区域 getter 就是它的开关 ——
 * 「当前可见且可滚动」的页面返回整屏矩形，其余页面返回 null，本帧不再接管
 * 新落下的触点（官方 ownership 规则在按下沿判定）。
 *
 * 不能每个页面各挂一个手势：官方优先级即注册顺序、后注册者抢先，后建的页面
 * 会一直抢走可见页的滚动；而 dispose 后重注册会连带取消进行中的触点。
 *
 * 滚动位移每帧直接写给内容节点的 translateY（官方 jump），不经过响应式绑定：
 * 官方把「信号写入 → 副作用重跑 → JSX 绑定 → 原生属性」这条路径标为毫秒级
 * （框架 hot.ts / apps/clear 的注释），逐帧走这条路用不起；jump 一次只有
 * 一个 FFI 调用，且 translate 是 paint-only，不触发重新布局。
 */
import { onScopeDispose, watchEffect } from 'vue';
import { jump } from '@pocketjs/framework/vue-vapor/animation';
import { attachGesture, type GestureHandle } from '@pocketjs/framework/vue-vapor/gesture';
import { createScroller, type Scroller } from '@pocketjs/framework/vue-vapor/kinetics';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { getFocused } from '@pocketjs/framework/vue-vapor/input';
import type { NodeMirror } from '@pocketjs/framework/vue-vapor/components';

/** 页面视口：状态栏是覆盖层，页面在整屏 240 × 280 内滚动。 */
const PAGE_REGION = { x: 0, y: 0, w: 240, h: 280 };
const PAGE_VIEW_H = PAGE_REGION.h;

/**
 * 焦点行：可聚焦控件在内容坐标里的位置。手柄操控时页面靠它把被聚焦的行滚进
 * 可视带——方向键移动焦点不产生任何触摸事件，页面不跟着走的话焦点环会停在
 * 底栏底下甚至屏幕外，用户看不到自己选中了哪一项。
 */
export interface FocusRow {
  /** 该行的焦点节点（框架聚焦的就是它）。 */
  node: NodeMirror | null;
  /** 行顶部在内容坐标里的 y。 */
  y: number;
  /** 行高。 */
  h: number;
}

/** 焦点行要落进的可视带：状态栏覆盖层之下、底栏之上各留一点余量。 */
const REVEAL_TOP = 34;
const REVEAL_BOTTOM = 200;
/** 跟随滚动的时长：短到不拖沓，长到看得出方向。 */
const REVEAL_MS = 140;

/** 焦点停在内容最后一项后再按一次下，页面自己往下走的距离（约一行）。 */
const PAD_SCROLL_STEP = 40;

interface Entry {
  active: () => boolean;
  scroller: Scroller;
  release: (velocity: number) => void;
  /** 手柄操控：焦点已经在最后一项时再按下，页面继续往下走一段。 */
  padScroll: () => void;
}

let handle: GestureHandle | null = null;
/** 当前接管手势的页面。 */
let owner: Entry | null = null;
/** 本次触点归谁：途中切页也不会把后续位移喂给新页面。 */
let dragging: Entry | null = null;

/** 松手收尾：把位移交回按下时记住的那一页。 */
function endDrag(velocity: number): void {
  const entry = dragging;
  dragging = null;
  entry?.release(velocity);
}

/** 懒注册：整应用一个识别器，区域 getter 决定它当前是否接管触点。 */
function gesture(): GestureHandle {
  if (handle === null) {
    handle = attachGesture({
      axis: 'y',
      region: { rect: () => (owner === null ? null : PAGE_REGION) },
      onDown: () => owner?.scroller.stop(),
      onPanStart: () => {
        dragging = owner;
        dragging?.scroller.beginDrag();
      },
      onPanMove: (c) => dragging?.scroller.drag(-c.fdy),
      onPanEnd: (c) => endDrag(-c.vy),
      onCancel: () => {
        if (dragging !== null && dragging.scroller.state() === 'tracking') {
          dragging.release(0);
        }
        dragging = null;
      },
    });
  }
  return handle;
}

/** 手势之外的第二条滚动入口：手柄操控时焦点停在内容末尾，再按下就让页面继续
 *  往下走（见 usePadControl 的方向分工）。当前接管手势的那一页就是它作用的对象；
 *  页面不可滚动时（owner 为 null）什么也不做。 */
export function padScrollPage(): void {
  if (owner !== null) {
    owner.padScroll();
  }
}

/**
 * @param active 页面是否可见
 * @param scrollable 页面能不能滚动：由调用方一次定死，不再从内容高度推导
 * @param contentH 内容高度（含末尾垫高）：可滚动页必传，用于夹住滚动范围
 * @param focusRows 可聚焦行的位置表（手柄操控时用来把焦点滚进可视带；
 *                  页面里全是静态等高行时给一份，控件位置不固定可不传）
 * @returns 内容列的 nodeRef 回调：页面把它挂在滚动列的根节点上
 */
export function usePageScroll(
  active: () => boolean,
  scrollable: boolean,
  contentH?: () => number,
  focusRows?: () => FocusRow[],
): (node: NodeMirror | null) => void {
  const maxOffset = () =>
    scrollable && contentH !== undefined ? Math.max(0, contentH() - PAGE_VIEW_H) : 0;
  // overscroll 0：拖拽在边缘硬夹住（官方默认 48 是橡皮筋行程）；extent 是橡皮筋的
  // 渐近线，固定为页面视口。
  const scroller = createScroller({
    max: maxOffset,
    extent: () => PAGE_VIEW_H,
    overscroll: 0,
  });

  /**
   * 松手：落点越界的抛掷改写成到边界的补间。官方的 snap 钩子是同一位置的入口，
   * 但它对每次松手都生效；这里只接管越界的那部分，界内仍走原生 fling。框架的
   * fling 撞到边缘会转交边缘弹簧，冲过边界一段再弹回。
   */
  const release = (velocity: number) => {
    scroller.endDrag(velocity);
    if (scroller.state() !== 'fling') {
      return;
    }
    const max = maxOffset();
    const projected = scroller.projectFling(velocity);
    if (projected >= 0 && projected <= max) {
      return;
    }
    const bound = projected < 0 ? 0 : max;
    const distance = Math.abs(bound - scroller.offset());
    // 三次缓出的起始速度是 3·距离/时长，取能对齐松手速度的时长，停下前不产生二次加速；
    // 上界避免慢速甩动被拖长。
    const speed = Math.max(Math.abs(velocity), 1);
    const durMs = Math.min(1000, Math.max(150, (3000 * distance) / speed));
    scroller.scrollTo(bound, { durMs });
  };

  /** 上一次请求的跟随滚动目标：目标不变就不重复下指令（跟随滚动与手柄操控的
   *  额外下移共用同一个目标值）。 */
  let revealTarget = Number.NaN;
  /** 被「额外下移」推出可视带的焦点行：位移是用户自己按下去的，跟随滚动不再
   *  把它拉回来，直到焦点换人或离开这一页。 */
  let padScrolled: NodeMirror | null = null;

  /**
   * 焦点已经在内容最后一项时再按下：页面自己往下走一段，一次一行的量，一直
   * 走到页底。此时焦点行会移出可视带，靠 padScrolled 让跟随滚动先让位。
   */
  const padScroll = () => {
    if (!scrollable || dragging === entry) {
      return;
    }
    const max = maxOffset();
    const current = scroller.offset();
    const target = Math.min(current + PAD_SCROLL_STEP, max);
    if (target <= current) {
      return;
    }
    padScrolled = getFocused();
    revealTarget = target;
    scroller.scrollTo(target, { durMs: REVEAL_MS });
  };

  const entry: Entry = { active, scroller, release, padScroll };
  gesture();

  /* 内容节点与已经写过的位移：节点换人就标成未知，下一帧重新写一次。 */
  let content: NodeMirror | null = null;
  let painted = Number.NaN;
  const contentRef = (node: NodeMirror | null) => {
    content = node;
    painted = Number.NaN;
  };

  /**
   * 手柄操控时把被聚焦的行滚进可视带：方向键移动焦点不产生触摸事件，页面
   * 不跟着走的话焦点环会停在底栏底下甚至屏幕外，用户看不到自己选中了哪一项。
   * 拖动期间不抢滚动；行已经在带里就不动，滚的是「刚好够」的距离。
   */
  const reveal = () => {
    if (focusRows === undefined || dragging === entry) {
      return;
    }
    const focused = getFocused();
    if (focused === null) {
      return;
    }
    if (padScrolled === focused) {
      return; // 这一行是用户按下的额外下移推出视野的，跟随滚动让位
    }
    padScrolled = null;
    const row = focusRows().find((candidate) => candidate.node === focused);
    if (row === undefined) {
      return;
    }
    const current = scroller.offset();
    let target = current;
    if (row.y + row.h - current > REVEAL_BOTTOM) {
      target = row.y + row.h - REVEAL_BOTTOM;
    }
    if (row.y - target < REVEAL_TOP) {
      target = row.y - REVEAL_TOP;
    }
    target = Math.min(Math.max(target, 0), maxOffset());
    if (target === current) {
      revealTarget = Number.NaN;
      return;
    }
    if (target !== revealTarget) {
      revealTarget = target;
      scroller.scrollTo(target, { durMs: REVEAL_MS });
    }
  };

  // 可见性变化只切换接管权；可滚动性由调用方一次定死。
  watchEffect(() => {
    if (active() && scrollable) {
      owner = entry;
    } else if (owner === entry) {
      owner = null;
    }
  });
  onScopeDispose(() => {
    if (owner === entry) {
      owner = null;
    }
    if (dragging === entry) {
      dragging = null;
    }
  });

  onFrame(() => {
    scroller.step();
    reveal();
    if (content === null) {
      return;
    }
    const offset = scroller.offset();
    if (offset !== painted) {
      painted = offset;
      jump(content, 'translateY', -offset);
    }
  });
  return contentRef;
}
