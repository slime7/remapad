// 浏览器端到端：动作注入按固件语义结算，弹窗 / 遮罩 / 焦点循环与控制台读数。
// 动作走 remapad.action（与固件 ui_service 的动作同名同参），断言三层：
// 快照语义事实、取样点比色、控制台最后一行的结算读数（log）。
import {
  BACKGROUND,
  BOX_POINT,
  CORNER_POINT,
  DIALOG_BOX,
  DEVICE_GRID,
  FILL_POINT,
  MASK_BLACK,
  SLOT_POINT,
  TRACK_FILL,
  TRACK_SLOT,
  activate,
  action,
  expect,
  focusStep,
  gridDiff,
  pixelGrid,
  px,
  setState,
  snapshot,
  test,
  waitPx,
  waitPxNot,
  waitState,
} from "./helpers.mjs";

test.describe("动作结算", () => {
  test("亮度加减在量程内钳位", async ({ page }) => {
    await action(page, "brightness-up");
    expect((await snapshot(page)).backlight).toBe(80);
    await waitPx(page, ...FILL_POINT, TRACK_FILL, 5000, "亮度 80 的填充");
    for (let i = 0; i < 4; i += 1) {
      await action(page, "brightness-down");
    }
    expect((await snapshot(page)).backlight).toBe(20);
    await waitPx(page, ...SLOT_POINT, TRACK_SLOT, 5000, "亮度 20 的空槽");
  });

  test("翻页动作在页表两端回绕", async ({ page }) => {
    await action(page, "next-page");
    expect((await snapshot(page)).page).toBe(1);
    await action(page, "prev-page");
    await action(page, "prev-page");
    expect((await snapshot(page)).page).toBe(8);
  });

  test("确认弹窗压暗整屏，确认键取消关闭", async ({ page }) => {
    const before = await pixelGrid(page, ...DEVICE_GRID);
    expect(await px(page, ...CORNER_POINT)).toEqual(BACKGROUND);
    await action(page, "ask-reboot");
    expect((await snapshot(page)).dialog).toBe(1);
    await waitPx(page, ...BOX_POINT, DIALOG_BOX, 5000, "弹窗盒面");
    const after = await pixelGrid(page, ...DEVICE_GRID);
    expect(gridDiff(before, after)).toBeGreaterThan(30);
    await activate(page);
    expect((await snapshot(page)).dialog).toBe(0);
  });

  test("确认重启后遮罩盖屏，随后自动收起", async ({ page }) => {
    await action(page, "ask-reboot");
    await setState(page, { dialogFocus: 1 });
    await activate(page);
    await waitPx(page, ...CORNER_POINT, MASK_BLACK, 5000, "重启遮罩左上");
    await waitPx(page, ...BOX_POINT, MASK_BLACK, 5000, "重启遮罩中部");
    expect((await snapshot(page)).rebooting).toBe(true);
    await waitState(page, "rebooting", false, 4000);
    await waitPxNot(page, ...CORNER_POINT, MASK_BLACK, 5000, "遮罩收起");
  });

  test("确认关机后遮罩盖屏，随后自动收起", async ({ page }) => {
    await action(page, "ask-power-off");
    await setState(page, { dialogFocus: 1 });
    await activate(page);
    await waitPx(page, ...BOX_POINT, MASK_BLACK, 5000, "关机遮罩");
    expect((await snapshot(page)).poweringOff).toBe(true);
    await waitState(page, "poweringOff", false, 4000);
    await waitPxNot(page, ...BOX_POINT, MASK_BLACK, 5000, "遮罩收起");
  });

  test("无线调试开关走动作结算并写读数", async ({ page }) => {
    await action(page, "netlog-toggle");
    let snap = await snapshot(page);
    expect(snap.netlogState).toBe(2);
    expect(snap.log).toContain("无线调试");
    await action(page, "netlog-toggle");
    expect((await snapshot(page)).netlogState).toBe(0);
  });

  test("主机格点击只在空闲档位开广播", async ({ page }) => {
    await action(page, "host-click");
    let snap = await snapshot(page);
    expect(snap.pairing).toBe(2);
    expect(snap.notice).toBe(0);
    await setState(page, { pairing: 0 });
    await action(page, "host-click");
    snap = await snapshot(page);
    expect(snap.pairing).toBe(2);
    expect(snap.notice).toBe(2);
  });

  test("焦点步进在当前页内循环", async ({ page }) => {
    await setState(page, { padActive: true, focusIndex: 0 });
    await focusStep(page, 1);
    expect((await snapshot(page)).focusIndex).toBe(1);
    await focusStep(page, 1);
    expect((await snapshot(page)).focusIndex).toBe(0);
    await focusStep(page, -1);
    expect((await snapshot(page)).focusIndex).toBe(1);
  });

  test("没有可聚焦项的页报告并归位", async ({ page }) => {
    await setState(page, { page: 6 });
    await focusStep(page, 1);
    const snap = await snapshot(page);
    expect(snap.focusIndex).toBe(-1);
    expect(snap.log).toContain("没有可聚焦项");
  });

  test("配色动作经快照读回", async ({ page }) => {
    await action(page, "next-page");
    await waitState(page, "page", 1);
    await action(page, "colorway", 3);
    expect((await snapshot(page)).selectedColorway).toBe(3);
    await page.screenshot({ path: test.info().outputPath("actions-colorway-3.png") });
  });
});
