// 浏览器端到端：真实指针事件（点按与拖动）经 canvas 进 Slint。
// 点按与拖动的语义宿主用例（gestures.rs / pages.rs）已经钉过，这里只验浏览器交付链路：
// winit web 后端把指针事件送进界面，控件按同样的规则响应。坐标含玻璃面 4px 黑边偏移。
import {
  FILL_POINT,
  HOST_CELL,
  MINUS_BUTTON,
  PLUS_BUTTON,
  SLOT_POINT,
  TRACK_FILL,
  TRACK_SLOT,
  drag,
  expect,
  setState,
  snapshot,
  tap,
  test,
  waitPx,
  waitState,
} from "./helpers.mjs";

test.describe("指针交互", () => {
  test("点按加号角钮调高亮度", async ({ page }) => {
    await tap(page, ...PLUS_BUTTON);
    expect((await snapshot(page)).backlight).toBe(80);
    await waitPx(page, ...FILL_POINT, TRACK_FILL, 5000, "亮度 80 的填充");
  });

  test("点按减号角钮调低亮度", async ({ page }) => {
    await setState(page, { backlight: 60 });
    await tap(page, ...MINUS_BUTTON);
    expect((await snapshot(page)).backlight).toBe(40);
    await waitPx(page, ...SLOT_POINT, TRACK_SLOT, 5000, "亮度 40 的空槽");
  });

  test("向左拖动整页滑行到下一张卡片", async ({ page }) => {
    await drag(page, 150, 100, 70, 100);
    await waitState(page, "page", 1, 4000);
    await page.screenshot({ path: test.info().outputPath("pointer-drag-page1.png") });
  });

  test("点按底栏主机格在空闲时开广播", async ({ page }) => {
    await setState(page, { pairing: 0 });
    await tap(page, ...HOST_CELL);
    const snap = await snapshot(page);
    expect(snap.pairing).toBe(2);
    expect(snap.notice).toBe(2);
  });
});
