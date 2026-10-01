// 浏览器端到端：物理屏外观与响应式布局。
// 玻璃面 248 × 280、内 40px 圆角；外面 8px 黑色边框、外 48px 圆角，两层同心；
// 宽屏时预览在左、控制台在右，窄屏时上下排。圆角裁剪用 elementFromPoint 验命中区域。
import { elementAtCanvasPoint, expect, test } from "./helpers.mjs";

test.describe("物理屏外观", () => {
  test("canvas 与玻璃面的几何与圆角", async ({ page }) => {
    const canvas = page.locator("#canvas");
    const bezel = page.locator(".bezel");
    const canvasBox = await canvas.boundingBox();
    expect(Math.round(canvasBox.width)).toBe(240);
    expect(Math.round(canvasBox.height)).toBe(280);
    expect(await canvas.evaluate((el) => getComputedStyle(el).borderTopLeftRadius)).toBe("40px");
    const bezelBox = await bezel.boundingBox();
    expect(Math.round(bezelBox.width)).toBe(256);
    expect(Math.round(bezelBox.height)).toBe(296);
    expect(await bezel.evaluate((el) => getComputedStyle(el).borderTopLeftRadius)).toBe("48px");
    expect(await bezel.evaluate((el) => getComputedStyle(el).paddingLeft)).toBe("8px");
  });

  test("内外圆角裁剪：弧内归画布、玻璃黑边归外框、框外归页面", async ({ page }) => {
    expect(await elementAtCanvasPoint(page, 30, 30)).toBe("canvas");
    expect(await elementAtCanvasPoint(page, 2, 2)).not.toBe("canvas");
    expect(await elementAtCanvasPoint(page, -4, 140)).toBe("bezel");
    expect(await elementAtCanvasPoint(page, -12, 140)).not.toBe("bezel");
  });
});

test.describe("宽屏布局（预览在左，控制台在右）", () => {
  test.use({ viewport: { width: 900, height: 700 } });

  test("预览与控制台横向并排", async ({ page }) => {
    const bezel = await page.locator(".bezel").boundingBox();
    const panel = await page.locator(".console").boundingBox();
    expect(bezel.x).toBeLessThan(panel.x);
    expect(bezel.x + bezel.width).toBeLessThanOrEqual(panel.x);
    const verticalOverlap = Math.min(bezel.y + bezel.height, panel.y + panel.height) - Math.max(bezel.y, panel.y);
    expect(verticalOverlap).toBeGreaterThan(0);
  });
});

test.describe("窄屏布局（预览在上，控制台在下）", () => {
  test.use({ viewport: { width: 360, height: 700 } });

  test("控制台折到预览下方", async ({ page }) => {
    const bezel = await page.locator(".bezel").boundingBox();
    const panel = await page.locator(".console").boundingBox();
    expect(bezel.y).toBeLessThan(panel.y);
    expect(bezel.y + bezel.height).toBeLessThanOrEqual(panel.y);
    const horizontalOverlap = Math.min(bezel.x + bezel.width, panel.x + panel.width) - Math.max(bezel.x, panel.x);
    expect(horizontalOverlap).toBeGreaterThan(0);
  });
});
