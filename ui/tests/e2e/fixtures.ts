/**
 * E2E 测试台：把预览页包装成「可触摸的屏幕」，用例用逻辑像素坐标操作，
 * 用组件树与像素事实断言。
 *
 * 三种事实来源，越靠前越推荐：
 *   1. 组件树（app.nodes / app.visibleTexts）——语义稳定，改样式不改行为；
 *   2. 屏幕像素（app.regionSignature / app.colorAt）——视觉与位移类断言；
 *   3. 预览页读数（app.readout）——触摸帧、命中节点等宿主侧事实。
 */
import { test as base, expect, type Page } from '@playwright/test';
import { installHarness } from './harness';

/** 设备视口，与 ui/pocket.json 的 app.viewport.logical 一致。 */
export const VIEW = { width: 240, height: 280 } as const;

/** 底部导航栏矩形（页面里是 left/right/bottom 各 8，高 64）。 */
export const NAV_BAR = { x: 8, y: 208, width: 224, height: 64 } as const;

/** 导航栏两个键的中心：左「状态」、右「设置」。 */
export const NAV_STATUS = { x: 84, y: 240 } as const;
export const NAV_SETTINGS = { x: 188, y: 240 } as const;

/**
 * 底部中区微型玩家指示灯：4 个 8px 方块（间距 3px），居中排在底栏中区图标下方。
 */
export const PLAYER_LED = {
  y: 256,
  xs: [104, 115, 126, 137],
  on: '#a6c8ff',
  off: '#002613',
} as const;

export interface FlatNode {
  i: number;
  t: string;
  n?: string;
  c?: string;
  x?: string;
  p: number;
  d: number;
  hidden: boolean;
}

export interface Rect {
  x: number;
  y: number;
  width: number;
  height: number;
}

interface PageApi {
  __remapadHarness?: {
    installed: boolean;
    send(command: Record<string, unknown>): void;
    requestTree(): void;
    frame(): number;
    waitFrames(count: number): Promise<number>;
    paused(): boolean;
    treeFrame(): number;
    treeReady(): boolean;
    nodes(): FlatNode[];
    visibleTexts(): string[];
    locate(ids: number[], step?: number): { x: number; y: number; width: number; height: number } | null;
    drain(): any[];
    peek(): any[];
  };
}

/** 触摸驱动：把 Playwright 的指针事件换成设备触点，坐标用逻辑像素。 */
export class TouchDriver {
  constructor(private readonly app: RemapadApp) {}

  /** 屏幕逻辑坐标 -> 页面坐标（画布可被 CSS 放大，触点按实际尺寸换算）。 */
  private async toClient(x: number, y: number): Promise<{ x: number; y: number }> {
    const box = await this.app.canvasBox();
    return {
      x: box.x + ((x + 0.5) * box.width) / VIEW.width,
      y: box.y + ((y + 0.5) * box.height) / VIEW.height,
    };
  }

  async tap(x: number, y: number, options: { holdMs?: number } = {}): Promise<void> {
    const at = await this.toClient(x, y);
    await this.app.page.mouse.move(at.x, at.y);
    await this.app.page.mouse.down();
    // 触点至少要跨过一次采样（预览页每帧采样一次）才会被应用看到。
    await this.app.waitFrames(2);
    if (options.holdMs !== undefined) {
      await this.app.page.waitForTimeout(options.holdMs);
    }
    await this.app.page.mouse.up();
    await this.app.waitFrames(2);
  }

  /**
   * 拖动：分步移动以便应用逐帧看到位移，最后按 dwellMs 停顿再抬手
   * （松手速度决定抛掷，停顿即「按住不动再放手」）。
   */
  async drag(
    from: { x: number; y: number },
    to: { x: number; y: number },
    options: { steps?: number; dwellMs?: number } = {},
  ): Promise<void> {
    const steps = options.steps ?? 12;
    const start = await this.toClient(from.x, from.y);
    const end = await this.toClient(to.x, to.y);
    await this.app.page.mouse.move(start.x, start.y);
    await this.app.page.mouse.down();
    await this.app.waitFrames(2);
    for (let step = 1; step <= steps; step += 1) {
      const ratio = step / steps;
      await this.app.page.mouse.move(
        start.x + (end.x - start.x) * ratio,
        start.y + (end.y - start.y) * ratio,
      );
      await this.app.waitFrames(1);
    }
    if (options.dwellMs !== undefined) {
      await this.app.page.waitForTimeout(options.dwellMs);
    }
    await this.app.page.mouse.up();
    await this.app.waitFrames(2);
  }

  /**
   * 甩动：整段位移在一两帧内走完并立刻抬手，触点速度足够触发惯性滚动。
   * 与 drag 的差别是收尾——抬手紧跟最后一次移动，中间不能等帧，否则触点
   * 速度被采样成 0，手势层只会停在手指位置。
   */
  async flick(
    from: { x: number; y: number },
    to: { x: number; y: number },
    options: { steps?: number; holdAfterMs?: number } = {},
  ): Promise<void> {
    const steps = options.steps ?? 6;
    const start = await this.toClient(from.x, from.y);
    const end = await this.toClient(to.x, to.y);
    await this.app.page.mouse.move(start.x, start.y);
    await this.app.page.mouse.down();
    await this.app.waitFrames(2);
    await this.app.page.mouse.move(end.x, end.y, { steps });
    if (options.holdAfterMs !== undefined) {
      await this.app.page.waitForTimeout(options.holdAfterMs);
    }
    await this.app.page.mouse.up();
    await this.app.waitFrames(2);
  }
}

/**
 * 手柄按键驱动：预览页把键盘当成设备按键位（方向键 / WASD 类比十字键，
 * 回车 / 空格类比圆圈键，见 ui/preview/index.html），真机上产生这些位的是
 * 手柄本身。按键至少要跨过一次采样帧，应用才看得到这次按下。
 */
export class PadDriver {
  constructor(private readonly app: RemapadApp) {}

  /** 按一次键（跨帧按住再松开）。 */
  async press(key: string, options: { holdFrames?: number } = {}): Promise<void> {
    await this.app.page.keyboard.down(key);
    await this.app.waitFrames(options.holdFrames ?? 2);
    await this.app.page.keyboard.up(key);
    await this.app.waitFrames(2);
  }

  /** 连按 n 次（每次都是独立的下沿：框架按帧做边沿检测）。 */
  async pressTimes(key: string, times: number): Promise<void> {
    for (let index = 0; index < times; index += 1) {
      await this.press(key);
    }
  }
}

export class RemapadApp {
  readonly touch: TouchDriver;
  readonly pad: PadDriver;
  /** 预览页的 console 日志（应用异常也会写进屏幕日志区）。 */
  readonly consoleLines: string[] = [];

  constructor(readonly page: Page) {
    this.touch = new TouchDriver(this);
    this.pad = new PadDriver(this);
    page.on('console', (message) => {
      this.consoleLines.push(`[${message.type()}] ${message.text()}`);
    });
    page.on('pageerror', (error) => {
      this.consoleLines.push(`[pageerror] ${error.message}`);
    });
  }

  /** 打开预览页并等待应用首帧就绪。 */
  async goto(): Promise<void> {
    await this.page.addInitScript(installHarness);
    await this.page.goto('/');
    await expect(this.page.locator('#stat-status')).toContainText('运行中', {
      timeout: 30_000,
    });
    // 首帧之后组件树需要一次快照，先要一份再交给用例。
    await this.refreshTree();
    await this.waitFrames(2);
  }

  /** 等待应用推进 n 帧（预览页按 host profile 的 tickHz 驱动帧循环）。 */
  async waitFrames(count: number): Promise<void> {
    await this.page.evaluate(async (frames) => {
      await globalThis.__remapadHarness?.waitFrames(frames);
    }, count);
  }

  /** 等待应用进入静止：连续两帧之间不再有绘制变化。 */
  async waitSettled(options: { quietMs?: number } = {}): Promise<void> {
    const quietMs = options.quietMs ?? 150;
    let last = await this.regionSignature({ x: 0, y: 34, width: 240, height: 170 });
    let stableSince = Date.now();
    for (let attempt = 0; attempt < 60; attempt += 1) {
      await this.page.waitForTimeout(50);
      const current = await this.regionSignature({ x: 0, y: 34, width: 240, height: 170 });
      if (current !== last) {
        last = current;
        stableSince = Date.now();
      } else if (Date.now() - stableSince >= quietMs) {
        return;
      }
    }
  }

  async frame(): Promise<number> {
    return this.page.evaluate(() => globalThis.__remapadHarness?.frame() ?? -1);
  }

  async canvasBox(): Promise<Rect> {
    const box = await this.page.locator('#screen').boundingBox();
    if (box === null) throw new Error('画布不可见，请检查 viewport 设置');
    return box;
  }

  /**
   * 重新取一份组件树快照并等待它到达。
   *
   * 等「份数增加」：shim 每 30 帧（TREE_THROTTLE）才自动推一次树，改动靠这次
   * 请求带回来，请求与快照之间隔一个应用帧，因此要等新的一份到达再断言。
   */
  async refreshTree(): Promise<void> {
    const before = await this.page.evaluate(
      () => globalThis.__remapadHarness?.treeSeq() ?? 0,
    );
    await this.page.evaluate(() => globalThis.__remapadHarness?.requestTree());
    await expect
      .poll(() => this.page.evaluate(() => globalThis.__remapadHarness?.treeSeq() ?? 0), {
        timeout: 15_000,
        intervals: [30],
      })
      .toBeGreaterThan(before);
  }

  /** 当前组件树的扁平视图；发现新快照时自动重新展开。 */
  async nodes(): Promise<FlatNode[]> {
    return this.page.evaluate(() => globalThis.__remapadHarness?.nodes() ?? []);
  }

  /** 当前可见（未被 hidden 隐藏）的文本集合。 */
  async visibleTexts(): Promise<string[]> {
    return this.page.evaluate(() => globalThis.__remapadHarness?.visibleTexts() ?? []);
  }

  /** 第一个文本等于 text 的节点。 */
  async findByText(text: string): Promise<FlatNode | undefined> {
    const nodes = await this.nodes();
    return nodes.find((node) => node.x === text);
  }

  /** 当前可见且 class 含该片段的第一个节点（切页后用来取页面根节点）。 */
  async findVisibleByClass(fragment: string): Promise<FlatNode | undefined> {
    const nodes = await this.nodes();
    return nodes.find((node) => !node.hidden && node.c?.includes(fragment) === true);
  }

  /**
   * 滚动位移：内容列被视口裁掉多少像素。内容列上移 offset 后，官方
   * debugRect 报告的是它与屏幕的交集，因此 offset = 视口高 − 交集高。
   * 节点在最近一帧没被绘制时返回 null（缓慢滚动/静止时可能发生）。
   */
  async scrollOffset(contentNodeId: number, timeoutMs = 400): Promise<number | null> {
    const rect = await this.inspectRect(contentNodeId, { timeoutMs });
    if (rect === null || rect.y !== 0 || rect.height > VIEW.height) return null;
    return VIEW.height - rect.height;
  }

  /** 连续采样滚动位移，用于观察甩动轨迹：越界回弹会在这里露出峰值。 */
  async sampleScroll(contentNodeId: number, durationMs: number): Promise<number[]> {
    const samples: number[] = [];
    const deadline = Date.now() + durationMs;
    while (Date.now() < deadline) {
      const rect = await this.inspectRect(contentNodeId, { timeoutMs: 300, keepTint: true });
      if (rect !== null && rect.y === 0 && rect.height <= VIEW.height) {
        samples.push(VIEW.height - rect.height);
      }
    }
    await this.clearInspectTint();
    return samples;
  }

  /** 屏幕上是否可见某段文本。 */
  async hasVisibleText(text: string): Promise<boolean> {
    return (await this.visibleTexts()).includes(text);
  }

  /** 向应用派发模拟的原生/固件 Bridge 事件。 */
  async emitBridge(msg: Record<string, unknown>): Promise<void> {
    await this.page.evaluate((m) => {
      (globalThis as any).__onNativeBridgeMessage?.(m);
    }, msg);
    await this.refreshTree();
  }

  /**
   * 按文本点下去：用官方边界命中扫描出这段文字当前位于最上层的那块区域，
   * 再点它的中心。适合按钮文字这类位置由布局决定、又不想在用例里写死坐标
   * 的场景；命中判定与触摸按下完全一致，所以「定位到」就等于「点得到」。
   */
  async tapText(text: string): Promise<void> {
    const node = await this.findByText(text);
    expect(node, `没找到文本「${text}」`).toBeDefined();
    await this.tapNode(node!);
  }

  /** 点某个节点当前可点区域的中心（含其祖先与后代，见 harness.locate）。 */
  async tapNode(node: FlatNode): Promise<void> {
    const nodes = await this.nodes();
    // 优先顺序：自己 → 最近的祖先一路到根 → 自己的后代。命中返回元素节点，
    // 文本子节点要由包住它的元素代收，所以最近的祖先排在前面（见 harness.locate）。
    const ids = [node.i];
    let current: FlatNode | undefined = node;
    while (current !== undefined && current.p >= 0) {
      current = nodes[current.p];
      if (current !== undefined) ids.push(current.i);
    }
    const own = new Set([node.i]);
    for (const candidate of nodes) {
      if (candidate.p >= 0 && own.has(nodes[candidate.p].i) && !own.has(candidate.i)) {
        own.add(candidate.i);
        ids.push(candidate.i);
      }
    }
    const box = await this.page.evaluate(
      (targets) => globalThis.__remapadHarness?.locate(targets) ?? null,
      ids,
    );
    expect(box, `节点 ${node.i}（${node.x ?? node.c ?? node.t}）当前点不到`).not.toBeNull();
    await this.touch.tap(box!.x + box!.width / 2, box!.y + box!.height / 2);
  }

  /** 节点的祖先链（含自身），从根到自身。 */
  async ancestors(node: FlatNode): Promise<FlatNode[]> {
    const nodes = await this.nodes();
    const chain: FlatNode[] = [];
    let current: FlatNode | undefined = node;
    while (current !== undefined) {
      chain.unshift(current);
      current = current.p >= 0 ? nodes[current.p] : undefined;
    }
    return chain;
  }

  /** 预览页读数面板。 */
  async readout(): Promise<{
    status: string;
    fps: string;
    touch: string;
    touchFrames: string;
    hit: string;
    keys: string;
    log: string;
  }> {
    return {
      status: (await this.page.locator('#stat-status').textContent()) ?? '',
      fps: (await this.page.locator('#stat-fps').textContent()) ?? '',
      touch: (await this.page.locator('#stat-touch').textContent()) ?? '',
      touchFrames: (await this.page.locator('#stat-frames').textContent()) ?? '',
      hit: (await this.page.locator('#stat-hit').textContent()) ?? '',
      keys: (await this.page.locator('#stat-keys').textContent()) ?? '',
      log: (await this.page.locator('#log').textContent()) ?? '',
    };
  }

  /** 逻辑像素处的颜色，返回 #rrggbb。 */
  async colorAt(x: number, y: number): Promise<string> {
    return this.page.evaluate(
      ([px, py]) => {
        const canvas = document.getElementById('screen') as HTMLCanvasElement;
        const context = canvas.getContext('2d')!;
        const data = context.getImageData(px, py, 1, 1).data;
        const hex = (value: number) => value.toString(16).padStart(2, '0');
        return `#${hex(data[0])}${hex(data[1])}${hex(data[2])}`;
      },
      [x, y],
    );
  }

  /**
   * 区域指纹：区域内像素的 FNV-1a 摘要。用于「这块画面有没有变」这类
   * 断言，避免把整幅截图当作基线（改样式不算回归）。
   */
  async regionSignature(rect: Rect): Promise<string> {
    return this.page.evaluate((area) => {
      const canvas = document.getElementById('screen') as HTMLCanvasElement;
      const context = canvas.getContext('2d')!;
      const data = context.getImageData(area.x, area.y, area.width, area.height).data;
      let hash = 0x811c9dc5;
      for (let index = 0; index < data.length; index += 1) {
        hash ^= data[index];
        hash = Math.imul(hash, 0x01000193) >>> 0;
      }
      return hash.toString(16).padStart(8, '0');
    }, rect);
  }

  /** 区域内与给定颜色相近的像素占比，用于「这里画出了东西」的粗判定。 */
  async colorShare(rect: Rect, color: string, tolerance = 12): Promise<number> {
    return this.page.evaluate(
      ({ area, target, tol }) => {
        const canvas = document.getElementById('screen') as HTMLCanvasElement;
        const context = canvas.getContext('2d')!;
        const data = context.getImageData(area.x, area.y, area.width, area.height).data;
        const want = [
          parseInt(target.slice(1, 3), 16),
          parseInt(target.slice(3, 5), 16),
          parseInt(target.slice(5, 7), 16),
        ];
        let hits = 0;
        const total = data.length / 4;
        for (let index = 0; index < data.length; index += 4) {
          if (
            Math.abs(data[index] - want[0]) <= tol &&
            Math.abs(data[index + 1] - want[1]) <= tol &&
            Math.abs(data[index + 2] - want[2]) <= tol
          ) {
            hits += 1;
          }
        }
        return hits / total;
      },
      { area: rect, target: color, tol: tolerance },
    );
  }

  /**
   * 区域内「亮到发白」的像素占比：焦点环是 2px 白色描边，圆角与斜边上的
   * 像素经过抗锯齿后落在 200 上下，按精确色判会漏掉大半，判亮度更稳。
   */
  async brightShare(rect: Rect, threshold = 200): Promise<number> {
    return this.page.evaluate(
      ({ area, level }) => {
        const canvas = document.getElementById('screen') as HTMLCanvasElement;
        const context = canvas.getContext('2d')!;
        const data = context.getImageData(area.x, area.y, area.width, area.height).data;
        let hits = 0;
        for (let index = 0; index < data.length; index += 4) {
          if (data[index] >= level && data[index + 1] >= level && data[index + 2] >= level) {
            hits += 1;
          }
        }
        return hits / (data.length / 4);
      },
      { area: rect, level: threshold },
    );
  }

  /**
   * 请求一次节点矩形（官方 inspect 调试操作），未绘制时返回 null。
   *
   * 官方 inspect 会给整幅画面加一层调试着色（像素整体变亮），着色在
   * inspect(0) 之前不会消失。所以这里读完矩形默认立刻清掉并等两帧重绘，
   * 否则同一用例里后续的像素断言会拿到着色过的颜色；连续采样时传
   * keepTint，由调用方在末尾统一清一次（见 sampleScroll）。
   */
  async inspectRect(
    nodeId: number,
    options: { timeoutMs?: number; keepTint?: boolean; clear?: boolean } = {},
  ): Promise<Rect | null> {
    // 先清空历史消息，避免读到上一次请求的矩形。
    if (options.clear !== false) {
      await this.page.evaluate(() => {
        globalThis.__remapadHarness?.drain();
      });
    }
    await this.page.evaluate((id) => {
      globalThis.__remapadHarness?.send({ t: 'inspect', id });
    }, nodeId);
    let latest: unknown = null;
    await expect
      .poll(
        async () => {
          latest = await this.page.evaluate((id) => {
            const messages = globalThis.__remapadHarness?.peek() ?? [];
            for (let index = messages.length - 1; index >= 0; index -= 1) {
              const message = messages[index];
              if (message.t === 'inspect' && message.id === id) return message;
            }
            return null;
          }, nodeId);
          return latest === null ? 'pending' : 'ready';
        },
        { timeout: options.timeoutMs ?? 10_000, intervals: [20] },
      )
      .toBe('ready');
    if (options.keepTint !== true) {
      await this.clearInspectTint();
    }
    const message = latest as { rect: [number, number, number, number] | null };
    if (message.rect === null) return null;
    const [x, y, width, height] = message.rect;
    return { x, y, width, height };
  }

  /** 关掉官方 inspect 的整屏调试着色，并等它重绘完。 */
  async clearInspectTint(): Promise<void> {
    await this.page.evaluate(() => {
      globalThis.__remapadHarness?.send({ t: 'inspect', id: 0 });
    });
    await this.waitFrames(2);
  }

  /** 应用内的 console 镜像（官方 shim 把 console.* 转发到通道）。 */
  async appConsole(): Promise<Array<{ level: string; args: string[] }>> {
    return this.page.evaluate(() => {
      const messages = globalThis.__remapadHarness?.drain() ?? [];
      return messages
        .filter((message) => message.t === 'log')
        .map((message) => ({ level: String(message.level), args: message.args as string[] }));
    });
  }
}

declare global {
  interface Window {
    __remapadHarness?: PageApi['__remapadHarness'];
  }
}

export const test = base.extend<{ app: RemapadApp }>({
  app: async ({ page }, use) => {
    await use(new RemapadApp(page));
  },
});

export { expect };
