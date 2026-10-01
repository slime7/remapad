// 浏览器端到端：预览页装载、首帧上屏、初始读数与物理屏外观。
// 被测对象是浏览器里跑的完整 WASM 预览（设备画面与动作结算在 ui/preview/preview-core.slint）；
// canvas 是设备屏本身，240 × 280 与面板 1:1；玻璃面（左右各 4px 黑边）与圆角是外面的 CSS。
import {
  BACKGROUND,
  FILL_POINT,
  SLOT_POINT,
  TRACK_FILL,
  TRACK_SLOT,
  expect,
  px,
  test,
  waitPx,
} from "./helpers.mjs";

test.describe("装载与首帧", () => {
  test("canvas 元素尺寸是 240 × 280（设备屏 1:1）", async ({ page }) => {
    const canvas = page.locator("#canvas");
    const box = await canvas.boundingBox();
    expect(Math.round(box.width)).toBe(240);
    expect(Math.round(box.height)).toBe(280);
    expect(await page.evaluate(() => document.getElementById("canvas").width)).toBe(240);
    expect(await page.evaluate(() => document.getElementById("canvas").height)).toBe(280);
    expect(await page.evaluate(() => document.getElementById("canvas").clientWidth)).toBe(240);
    expect(await page.evaluate(() => document.getElementById("canvas").clientHeight)).toBe(280);
    expect(await canvas.evaluate((el) => getComputedStyle(el).width)).toBe("240px");
    expect(await canvas.evaluate((el) => getComputedStyle(el).height)).toBe("280px");
  });

  test("玻璃面 8px 黑边包住 canvas", async ({ page }) => {
    const bezel = page.locator(".bezel");
    const box = await bezel.boundingBox();
    expect(Math.round(box.width)).toBe(256);
    expect(Math.round(box.height)).toBe(296);
    for (const side of ["Top", "Right", "Bottom", "Left"]) {
      expect(await bezel.evaluate((el, s) => getComputedStyle(el)[`padding${s}`], side)).toBe("8px");
    }
  });

  test("快照给出预览默认值", async ({ page }) => {
    const snap = await page.evaluate(() => window.remapad.snapshot());
    expect(snap.page).toBe(0);
    expect(snap.pageCount).toBe(9);
    expect(snap.focusCount).toBe(2);
    expect(snap.backlight).toBe(60);
    expect(snap.dialog).toBe(0);
  });

  test("首帧画出亮度页", async ({ page }) => {
    await waitPx(page, ...FILL_POINT, TRACK_FILL, 5000, "亮度滑槽填充");
    await waitPx(page, ...SLOT_POINT, TRACK_SLOT, 5000, "亮度滑槽空段");
    await page.screenshot({ path: test.info().outputPath("smoke-initial.png") });
  });

  test("设备画面左上角是背景色", async ({ page }) => {
    await waitPx(page, 5, 5, BACKGROUND, 5000, "画面左上角背景");
  });
});

test.describe("小数缩放（Windows 125%）", () => {
  test.use({ deviceScaleFactor: 1.25 });

  test("预览照常启动，后备缓冲保持 1:1 逻辑像素", async ({ page }) => {
    expect(await page.evaluate(() => window.__remapadReady)).toBe(true);
    expect(
      await page.evaluate(() => {
        const c = document.getElementById("canvas");
        return [c.width, c.height, c.clientWidth, c.clientHeight];
      }),
    ).toEqual([240, 280, 240, 280]);
    // 缩放因子钉在 1：后备缓冲与 CSS 坐标一一对应，画面左上角仍是背景色。
    const corner = await page.evaluate(() => {
      const d = document.getElementById("canvas").getContext("2d").getImageData(5, 5, 1, 1).data;
      return [d[0], d[1], d[2]];
    });
    expect(corner).toEqual(BACKGROUND);
  });
});
