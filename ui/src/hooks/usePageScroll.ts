/**
 * 页面级动能滚动：全应用共用一个纵向手势，只有「当前可见且内容可滚动」的
 * 页面才挂载它（enable/disable 切换），其余页面连 claim 都不会发生。
 *
 * 不能每个页面各挂一个手势：全屏手势之间按「最后注册者优先」竞争 pan
 * claim，后注册的页面会永远抢走可见页的滚动。
 */
import { onScopeDispose, watchEffect } from 'vue';
import { attachGesture, type GestureHandle } from '@pocketjs/framework/vue-vapor/gesture';
import { createScroller, type Scroller } from '@pocketjs/framework/vue-vapor/kinetics';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';

/** 页面视口高度：状态栏是覆盖层，页面在整屏 280 内滚动。 */
const PAGE_VIEW_H = 280;

interface Entry {
  active: () => boolean;
  scroller: Scroller;
  attach: () => GestureHandle;
}

const entries: Entry[] = [];
let handle: GestureHandle | null = null;
let owner: Entry | null = null;

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
  // overscroll 0：拖拽在边缘硬夹住，没有橡皮筋。
  const scroller = createScroller({ max: maxOffset, overscroll: 0 });

  /**
   * 松手：落点越界的抛掷改写成到边界的补间。框架的 fling 撞到边缘会转交
   * 边缘弹簧，实测会冲过边界约 70 px 再弹回；这里在松手瞬间换成确定性停止。
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

  const entry: Entry = {
    active,
    scroller,
    attach: () =>
      attachGesture({
        axis: 'y',
        onDown: () => scroller.stop(),
        onPanStart: () => scroller.beginDrag(),
        onPanMove: (c) => scroller.drag(-c.fdy),
        onPanEnd: (c) => release(-c.vy),
        onCancel: () => {
          if (scroller.state() === 'tracking') {
            release(0);
          }
        },
      }),
  };
  entries.push(entry);

  // 可见性变化时启停手势；可滚动性由调用方定死，不再随内容高度变化。
  watchEffect(() => {
    const wanted = active() && scrollable;
    if (wanted && owner !== entry) {
      handle?.dispose();
      handle = entry.attach();
      owner = entry;
    } else if (!wanted && owner === entry) {
      handle?.dispose();
      handle = null;
      owner = null;
    }
  });
  onScopeDispose(() => {
    const index = entries.indexOf(entry);
    if (index >= 0) {
      entries.splice(index, 1);
    }
    if (owner === entry) {
      handle?.dispose();
      handle = null;
      owner = null;
    }
  });

  onFrame(() => {
    scroller.step();
  });
  return scroller;
}
