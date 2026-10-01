// 浏览器端到端：HTML 控制台（预览之外的面板）。
// 控制台只发动作与写模拟状态，结算在 wasm 核心里；按钮标签与高亮随快照回填，
// 设备画面上的点按也要能在控制台读数里看到（轮询刷新）。
import { PLUS_BUTTON, activate, expect, setState, tap, test, waitState } from "./helpers.mjs";

test.describe("控制台", () => {
  test("翻页按钮切页并把结算文案写进 log", async ({ page }) => {
    await page.click("#btn-next-page");
    await waitState(page, "page", 1);
    await expect.poll(() => page.textContent("#console-log")).toContain("next-page");
    await expect.poll(() => page.textContent("#console-log")).toContain("next-page(0)");
  });

  test("手柄操控开关打开焦点环并高亮按钮", async ({ page }) => {
    await page.click("#btn-pad-active");
    await waitState(page, "padActive", true);
    await waitState(page, "focusIndex", 0);
    await expect(page.locator("#btn-pad-active")).toHaveClass(/active/);
  });

  test("配对档位从控制台直接写模拟状态", async ({ page }) => {
    await page.click("#btn-pairing-cycle");
    await waitState(page, "pairing", 3);
    await expect.poll(() => page.textContent("#console-log")).toContain("配对状态 = 3");
  });

  test("确认键走设备焦点分发", async ({ page }) => {
    await setState(page, { page: 3, padActive: true, focusIndex: 1 });
    await page.click("#btn-activate");
    await waitState(page, "dialog", 2);
  });

  test("WiFi 开关走动作结算并换标签", async ({ page }) => {
    await page.click("#btn-netlog");
    await waitState(page, "netlogState", 2);
    await expect(page.locator("#btn-netlog")).toHaveText("WiFi 开");
    await expect.poll(() => page.textContent("#console-log")).toContain("无线调试");
  });

  test("电量按钮按量程步进并回写读数", async ({ page }) => {
    await page.click("#btn-battery-up");
    await waitState(page, "batteryPercent", 86);
    await expect.poll(() => page.textContent("#console-log")).toContain("电量 = 86%");
  });

  test("USB 角色按钮随状态换标签，切回串口弹重启询问", async ({ page }) => {
    await expect(page.locator("#btn-usb-role")).toHaveText("USB 串口");
    await page.click("#btn-usb-role");
    await waitState(page, "usbRole", 1);
    await expect(page.locator("#btn-usb-role")).toHaveText("USB 手柄");
    await page.click("#btn-usb-role");
    await waitState(page, "dialog", 4);
  });

  test("设备画面点按也会回写控制台读数", async ({ page }) => {
    await tap(page, ...PLUS_BUTTON);
    await waitState(page, "backlight", 80);
    await expect.poll(() => page.textContent("#console-log"), { timeout: 5000 }).toContain("背光 = 80%");
  });

  test("读数行跟随快照字段", async ({ page }) => {
    await setState(page, { dialog: 2, focusIndex: 1, pairing: 4, playerLed: 5 });
    await activate(page); // dialogFocus 默认 0 → 取消，弹窗归零，其余字段留在读数里
    await expect
      .poll(() => page.textContent("#console-readout"))
      .toContain("弹窗 0 · 焦点 1 · 配对 4 · 灯 5");
  });
});
