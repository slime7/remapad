/**
 * 页面侧探针：由 Playwright 在预览页任何脚本执行前注入，接住应用发往
 * DevTools 的通道，从而把「组件树 / 帧号 / 控制台」变成测试可读的事实。
 *
 * 不修改 ui/preview 与 ui/src：探针只接管 globalThis.__pocketDevtoolsTransport
 * 这个官方约定的注入口（官方 framework/src/devtools.ts 读取它作为传输层）。
 * 页面自己的 connectDevtools 仍会连 8131，但写不进这个属性，WS 通道对测试
 * 不再有影响，用例因此不依赖 8131 的任何状态。
 *
 * 注意：该函数会被序列化后送进浏览器执行，必须自包含，不能引用外部变量。
 */
export function installHarness(): void {
  const MAX_MESSAGES = 3000;
  const global = globalThis as unknown as Record<string, any>;

  interface Entry {
    i: number;
    t: string;
    n?: string;
    c?: string;
    x?: string;
    /** 扁平数组里的父节点下标；根为 -1。 */
    p: number;
    d: number;
    hidden: boolean;
  }

  const messages: any[] = [];
  const queue: string[] = [];
  let lastTree: any = null;
  let lastTreeFrame = -1;
  /** 收到的树快照份数：用例靠它等待新的一份快照到达。 */
  let treeSeq = 0;
  let frame = 0;
  let paused = false;
  let flatCache: { source: any; nodes: Entry[] } | null = null;
  /** 应用帧计数与 bundle 装上的帧回调（见下面的 globalThis.frame 存取器）。 */
  let appFrames = 0;
  let frameHandler: ((...args: any[]) => unknown) | null = null;

  function record(line: string): void {
    let message: any = null;
    try {
      message = JSON.parse(line);
    } catch {
      return;
    }
    if (!message || typeof message !== "object") return;
    if (typeof message.frame === "number" && message.frame > frame) {
      frame = message.frame;
    }
    if (message.t === "stats" && typeof message.paused === "boolean") {
      paused = message.paused;
    }
    if (message.t === "tree") {
      lastTree = message.root ?? null;
      lastTreeFrame = typeof message.frame === "number" ? message.frame : frame;
      treeSeq += 1;
    }
    messages.push(message);
    if (messages.length > MAX_MESSAGES) {
      messages.splice(0, messages.length - MAX_MESSAGES);
    }
  }

  const transport = {
    send: (line: string) => record(line),
    recv: () => (queue.length > 0 ? queue.shift()! : null),
    everyFrames: 1,
  };

  // 页面稍后会自己给这个属性赋值（预览页的 DevTools 通道）；这里定义成
  // 只读访问器，赋值被吞掉，官方 shim 读到的始终是探针的传输层。
  Object.defineProperty(global, "__pocketDevtoolsTransport", {
    configurable: true,
    get: () => transport,
    set: () => {},
  });

  // 预览页从 bundle 装上的 globalThis.frame 逐帧驱动应用。这里包一层计数，
  // 让 waitFrames 数的是应用帧本身：宿主帧率由 host profile 的 tickHz 决定，
  // requestAnimationFrame 的节奏与它不同。
  Object.defineProperty(global, "frame", {
    configurable: true,
    get: () => frameHandler,
    set: (handler: unknown) => {
      if (typeof handler !== "function") {
        frameHandler = null;
        return;
      }
      frameHandler = (...args: unknown[]): unknown => {
        appFrames += 1;
        return (handler as (...rest: unknown[]) => unknown)(...args);
      };
    },
  });

  /** 「hidden」是页面切换可见性的类名；overflow-hidden 这类子串不算。 */
  function hasHiddenClass(node: any): boolean {
    return typeof node?.c === "string" && node.c.split(/\s+/).includes("hidden");
  }

  function flatten(root: any): Entry[] {
    const out: Entry[] = [];
    const visit = (node: any, parent: number, depth: number, hidden: boolean): void => {
      if (!node || typeof node !== "object") return;
      const nowHidden = hidden || hasHiddenClass(node);
      const index = out.length;
      out.push({
        i: node.i,
        t: node.t,
        n: node.n,
        c: node.c,
        x: node.x,
        p: parent,
        d: depth,
        hidden: nowHidden,
      });
      const children = node.k;
      if (Array.isArray(children)) {
        for (const child of children) visit(child, index, depth + 1, nowHidden);
      }
    };
    visit(root, -1, 0, false);
    return out;
  }

  global.__remapadHarness = {
    installed: true,
    send: (command: Record<string, unknown>) => {
      queue.push(JSON.stringify(command));
    },
    /** 请求一次组件树快照；下一次帧循环就会回发。 */
    requestTree: () => {
      queue.push(JSON.stringify({ t: "getTree" }));
    },
    frame: () => frame,
    /**
     * 等满 n 个应用帧。计数来自这里包住 globalThis.frame 的那一层，与宿主帧率
     * 无关；不要用 shim 每 30 帧才推一次的 stats.frame 计数，那个粒度会掩盖
     * 时序类回归。应用停帧时按墙钟兜底返回，用例带着自己的断言失败。
     */
    waitFrames: (count: number) => {
      const wanted = Math.max(0, Math.floor(count));
      const target = appFrames + wanted;
      const deadline = Date.now() + 5000 + wanted * 100;
      return new Promise<number>((resolve) => {
        const step = (): void => {
          if (appFrames >= target || Date.now() >= deadline) {
            resolve(appFrames);
            return;
          }
          requestAnimationFrame(step);
        };
        requestAnimationFrame(step);
      });
    },
    paused: () => paused,
    treeFrame: () => lastTreeFrame,
    treeSeq: () => treeSeq,
    treeReady: () => lastTree !== null,
    /** 扁平化后的组件树；同一份快照只展开一次。 */
    nodes: (): Entry[] => {
      if (lastTree === null) return [];
      if (flatCache === null || flatCache.source !== lastTree) {
        flatCache = { source: lastTree, nodes: flatten(lastTree) };
      }
      return flatCache.nodes;
    },
    /** 未被任何祖先隐藏的文本节点内容（按出现顺序去重）。 */
    visibleTexts: (): string[] => {
      const nodes = global.__remapadHarness.nodes() as Entry[];
      const texts: string[] = [];
      for (const node of nodes) {
        if (node.hidden || node.x === undefined || node.x === "") continue;
        if (!texts.includes(node.x)) texts.push(node.x);
      }
      return texts;
    },
    /** 取回并清空已收到的事件（测试用来观察 hello / stats / error）。 */
    drain: () => {
      const taken = messages.slice();
      messages.length = 0;
      return taken;
    },
    peek: () => messages.slice(),
    /**
     * 用官方边界命中（op 42）扫描屏幕，按 candidates 给的优先顺序，返回第一个
     * 真正能点到的节点的矩形。命中与触摸按下走同一条判定，所以这里返回的矩形
     * 就是可以点到的区域——不受增量重绘影响，静止画面也能定位。
     *
     * 命中返回的是元素节点：文本子节点自己从不参与命中，点它只能命中包住它的
     * Text/View 元素。调用方因此按「自己、最近的祖先、…、根、后代」排序，
     * 最近的祖先给出的才是紧贴目标的那块区域；若改按面积挑最小，两个按钮
     * 并排时可能选中包住它们的那一行，点下去正好落在按钮之间的空隙。
     */
    locate: (ids: number[], step = 4) => {
      const ui = (globalThis as unknown as { ui?: any }).ui;
      if (ui === undefined || typeof ui.hitTestBounds !== "function") return null;
      const order = new Map<number, number>();
      ids.forEach((id, index) => order.set(id, index));
      const view = ui.__viewport ?? { w: 240, h: 280 };
      const boxes = new Map<number, { x0: number; y0: number; x1: number; y1: number }>();
      for (let y = 0; y < view.h; y += step) {
        for (let x = 0; x < view.w; x += step) {
          const hit = ui.hitTestBounds(x, y);
          if (!order.has(hit)) continue;
          const box = boxes.get(hit);
          if (box === undefined) {
            boxes.set(hit, { x0: x, y0: y, x1: x, y1: y });
          } else {
            if (x < box.x0) box.x0 = x;
            if (y < box.y0) box.y0 = y;
            if (x > box.x1) box.x1 = x;
            if (y > box.y1) box.y1 = y;
          }
        }
      }
      let best: { x: number; y: number; width: number; height: number } | null = null;
      let bestRank = Infinity;
      for (const [id, box] of boxes) {
        const rank = order.get(id) ?? Infinity;
        if (rank >= bestRank) continue;
        bestRank = rank;
        const x = box.x0;
        const y = box.y0;
        best = {
          x,
          y,
          width: Math.min(view.w - x, box.x1 - x + step),
          height: Math.min(view.h - y, box.y1 - y + step),
        };
      }
      return best;
    },
  };
}
