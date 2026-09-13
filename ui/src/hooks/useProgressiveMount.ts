/**
 * 分帧挂载游标：把一次建树拆成若干块，每帧只放行一块。
 *
 * 实测单个原生节点的建树成本约 50 ms（240x280，见 ADR 0014），整页一次建完
 * 会把单帧拖到秒级，期间屏幕完全不刷新。用法是先建外层容器，再用
 * `cursor() >= n` 按从上到下的顺序逐帧放行兄弟节点：页面逐块出现，每一帧的
 * 阻塞时长被压到一块的量级。
 */
import { onScopeDispose, ref } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';

/** 仍在分帧填充的页面数：首页加载提示据此显示。 */
let fillingPages = 0;

export function pagesFilling(): boolean {
  return fillingPages > 0;
}

/** 返回当前已开放的块号（从 0 开始，每帧 +1，达到 total 后停在 total）。 */
export function useMountCursor(total: number): () => number {
  const step = ref(0);
  let open = total > 0;
  if (open) {
    fillingPages += 1;
  }
  const finish = () => {
    if (!open) {
      return;
    }
    open = false;
    fillingPages -= 1;
  };
  onFrame(() => {
    if (!open) {
      return;
    }
    if (step.value < total) {
      step.value += 1;
    }
    if (step.value >= total) {
      finish();
    }
  });
  onScopeDispose(finish);
  return () => step.value;
}
