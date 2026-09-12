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
  scrollable: () => boolean;
  scroller: Scroller;
  attach: () => GestureHandle;
}

const entries: Entry[] = [];
let handle: GestureHandle | null = null;
let owner: Entry | null = null;

export function usePageScroll(active: () => boolean, contentH: () => number) {
  const scroller = createScroller({ max: () => Math.max(0, contentH() - PAGE_VIEW_H) });
  const entry: Entry = {
    active,
    scrollable: () => Math.max(0, contentH() - PAGE_VIEW_H) > 0,
    scroller,
    attach: () =>
      attachGesture({
        axis: 'y',
        onDown: () => scroller.stop(),
        onPanStart: () => scroller.beginDrag(),
        onPanMove: (c) => scroller.drag(-c.fdy),
        onPanEnd: (c) => scroller.endDrag(-c.vy),
        onCancel: () => {
          if (scroller.state() === 'tracking') {
            scroller.endDrag(0);
          }
        },
      }),
  };
  entries.push(entry);

  // 可见性或可滚动性变化时启停手势；watchEffect 内读取 active()/contentH()
  // 建立跟踪，tab 切换或内容增删都会重新求值。
  watchEffect(() => {
    const wanted = active() && entry.scrollable();
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
