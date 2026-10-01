// 端到端用例公共工具：语义色与取样点、wasm 接口包装、像素取样与等待。
// 断言三层（与 ADR 0019 的口径一致）：快照读语义事实、canvas 取样点比色验「画出来了没有」、控制台 log 读结算文案。
// 取样点坐标是 canvas 上的绝对位置：canvas = 设备屏本身，240 × 280 与面板 1:1。
// page.evaluate 一律传真函数（Playwright 不自动调用字符串形式的箭头函数）。
import { expect, test as base } from "@playwright/test";

export { expect };

export const test = base.extend({
  // 每个用例开一个新页面（等价新预览实例），等就绪标记变真再开始断言。
  page: async ({ page }, use) => {
    await page.goto("/");
    await page.waitForFunction("window.__remapadReady === true");
    await use(page);
  },
});

// 语义色（ui/src/theme.slint 与组件底色，探针实测核对过）：
export const TRACK_FILL = [255, 255, 255]; // 亮度滑槽填充
export const TRACK_SLOT = [27, 65, 111]; // 亮度滑槽空段（on-primary-container）
export const BACKGROUND = [6, 14, 27]; // 背景（background）
export const DIALOG_BOX = [17, 32, 53]; // 弹窗盒面（surface-container-high）
export const MASK_BLACK = [0, 0, 0]; // 重启 / 关机等待遮罩

export const FILL_POINT = [74, 150]; // 亮度滑槽：默认背光 60 时填充盖住的点
export const SLOT_POINT = [74, 60]; // 亮度滑槽：背光 <= 70 时仍在空段上的点
export const CORNER_POINT = [5, 5]; // 设备画面左上角：无弹窗时是纯背景色
export const BOX_POINT = [120, 140]; // 弹窗盒面中部
export const PLUS_BUTTON = [166, 58]; // 亮度页加号角钮中心
export const MINUS_BUTTON = [166, 150]; // 亮度页减号角钮中心
export const HOST_CELL = [120, 240]; // 底栏主机格中心

// 设备画面区域的取样网格。
export const DEVICE_GRID = [0, 0, 239, 279, 20];

export function close(actual, expected, tol = 6) {
  return actual.every((channel, i) => Math.abs(channel - expected[i]) <= tol);
}

export function gridDiff(before, after, tol = 6) {
  return before.filter((pixel, i) => !close(pixel, after[i], tol)).length;
}

// ---- wasm 接口 ----

export async function snapshot(page) {
  return page.evaluate(() => window.remapad.snapshot());
}

export async function action(page, name, value = 0) {
  await page.evaluate(([n, v]) => window.remapad.action(n, v), [name, value]);
}

export async function focusStep(page, delta) {
  await page.evaluate((d) => window.remapad.focusStep(d), delta);
}

export async function activate(page) {
  await page.evaluate(() => window.remapad.activateFocused());
}

export async function setState(page, patch) {
  await page.evaluate((p) => window.remapad.setState(p), patch);
}

// ---- 像素取样 ----

export async function px(page, x, y) {
  return page.evaluate(
    ([px, py]) => {
      const d = document.getElementById("canvas").getContext("2d").getImageData(px, py, 1, 1).data;
      return [d[0], d[1], d[2]];
    },
    [x, y],
  );
}

export async function pixelGrid(page, x0, y0, x1, y1, step = 20) {
  return page.evaluate(
    ([gx0, gy0, gx1, gy1, gstep]) => {
      const ctx = document.getElementById("canvas").getContext("2d");
      const img = ctx.getImageData(gx0, gy0, gx1 - gx0 + 1, gy1 - gy0 + 1).data;
      const w = gx1 - gx0 + 1;
      const out = [];
      for (let y = gy0; y <= gy1; y += gstep) {
        for (let x = gx0; x <= gx1; x += gstep) {
          const i = ((y - gy0) * w + (x - gx0)) * 4;
          out.push([img[i], img[i + 1], img[i + 2]]);
        }
      }
      return out;
    },
    [x0, y0, x1, y1, step],
  );
}

export async function waitPx(page, x, y, expected, timeout = 5000, label = "") {
  const deadline = Date.now() + timeout;
  let actual = null;
  while (Date.now() < deadline) {
    actual = await px(page, x, y);
    if (close(actual, expected)) {
      return actual;
    }
    await page.waitForTimeout(60);
  }
  throw new Error(`${label || "像素"} (${x},${y}) 等 ${JSON.stringify(expected)} 超时，最后取值 ${JSON.stringify(actual)}`);
}

export async function waitPxNot(page, x, y, forbidden, timeout = 5000, label = "") {
  const deadline = Date.now() + timeout;
  let actual = null;
  while (Date.now() < deadline) {
    actual = await px(page, x, y);
    if (!close(actual, forbidden)) {
      return actual;
    }
    await page.waitForTimeout(60);
  }
  throw new Error(`${label || "像素"} (${x},${y}) 一直停在 ${JSON.stringify(forbidden)}，未按预期变化`);
}

export async function waitState(page, field, expected, timeout = 5000) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    const actual = (await snapshot(page))[field];
    if (actual === expected) {
      return actual;
    }
    await page.waitForTimeout(60);
  }
  throw new Error(`快照字段 ${field} 等 ${String(expected)} 超时，最后取值 ${(await snapshot(page))[field]}`);
}

// ---- 指针事件 ----

export async function canvasPoint(page, x, y) {
  const box = await page.locator("#canvas").boundingBox();
  return [box.x + x, box.y + y];
}

export async function tap(page, x, y) {
  const [vx, vy] = await canvasPoint(page, x, y);
  await page.mouse.click(vx, vy);
}

export async function drag(page, x0, y0, x1, y1, steps = 8) {
  const [ax, ay] = await canvasPoint(page, x0, y0);
  const [bx, by] = await canvasPoint(page, x1, y1);
  await page.mouse.move(ax, ay);
  await page.mouse.down();
  await page.mouse.move(bx, by, { steps });
  await page.mouse.up();
}

// canvas 坐标处的命中元素（id 或 class），圆角裁剪用例用它验弧内外的归属。
export async function elementAtCanvasPoint(page, x, y) {
  const [vx, vy] = await canvasPoint(page, x, y);
  return page.evaluate(
    ([px, py]) => {
      const el = document.elementFromPoint(px, py);
      return el ? el.id || el.className : null;
    },
    [vx, vy],
  );
}
