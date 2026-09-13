/**
 * 页面级动能滚动：全应用只注册一个纵向手势，区域 getter 就是它的开关 ——
 * 「当前可见且可滚动」的页面返回整屏矩形，其余页面返回 null，本帧不再接管
 * 新落下的触点（官方 ownership 规则在按下沿判定）。
 *
 * 不能每个页面各挂一个手势：官方优先级即注册顺序、后注册者抢先，后建的页面
 * 会一直抢走可见页的滚动；而 dispose 后重注册会连带取消进行中的触点。
 */
import { onScopeDispose, watchEffect } from 'vue';
import { attachGesture, type GestureHandle } from '@pocketjs/framework/vue-vapor/gesture';
import { createScroller, type Scroller } from '@pocketjs/framework/vue-vapor/kinetics';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';

/** 页面视口：状态栏是覆盖层，页面在整屏 240 × 280 内滚动。 */
const PAGE_REGION = { x: 0, y: 0, w: 240, h: 280 };
const PAGE_VIEW_H = PAGE_REGION.h;

interface Entry {
  active: () => boolean;
  scroller: Scroller;
  release: (velocity: number) => void;
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

/**
 * @param active 页面是否可见
 * @param scrollable 页面能不能滚动：由调用方一次定死，不再从内容高度推导
 * @param contentH 内容高度（含末尾垫高）：可滚动页必传，用于夹住滚动范围
 */
export function usePageScroll(
  active: () => boolean,
  scrollable: boolean,
  contentH?: () => number,
) {
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
   * fling 撞到边缘会转交边缘弹簧，实测会冲过边界约 70 px 再弹回。
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

  const entry: Entry = { active, scroller, release };
  gesture();

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
  });
  return scroller;
}
