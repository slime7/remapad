/**
 * 系统功能测试：
 * 1. 第 1 页：背光调节（20%~100% 步进）；
 * 2. 第 4 页：电源管理（重启与关机确认弹窗）；
 * 3. 第 5 页：系统信息（固件版本与内存）。
 */
import { test, expect } from './fixtures';
import { goHome, openPower, openSystemInfo } from './pages';

test('第 1 页亮度调节步进（1-5 档），最低不会降到 0 档', async ({ app }) => {
  await app.goto();
  await goHome(app);
  expect(await app.hasVisibleText('2')).toBe(true);

  // 点击加号增亮 (2档 -> 3档)
  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);

  // 点击减号减暗 (3档 -> 2档)
  await app.tapText('−');
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);

  // 连续减暗：最低停在 1 档，不会降到 0 档黑屏
  for (let i = 0; i < 4; i++) {
    await app.tapText('−');
  }
  await expect.poll(() => app.hasVisibleText('1')).toBe(true);
  expect(await app.hasVisibleText('0')).toBe(false);
});

test('第 4 页电源管理：点击重启唤起确认弹窗，取消可关闭', async ({ app }) => {
  await app.goto();
  await openPower(app);

  await app.tapText('重启设备');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);
  expect(await app.hasVisibleText('取消')).toBe(true);
  expect(await app.hasVisibleText('重启')).toBe(true);

  // 点击取消关闭弹窗
  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(false);
});

test('第 4 页电源管理：点击关机唤起确认弹窗', async ({ app }) => {
  await app.goto();
  await openPower(app);

  await app.tapText('设备关机');
  await expect.poll(() => app.hasVisibleText('是否立即关闭 Remapad 设备？')).toBe(true);
  expect(await app.hasVisibleText('关机')).toBe(true);

  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('是否立即关闭 Remapad 设备？')).toBe(false);
});

test('第 5 页系统信息展示固件与内存参数', async ({ app }) => {
  await app.goto();
  await openSystemInfo(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('设备信息');
  expect(texts.some((t) => t.startsWith('固件:'))).toBe(true);
  expect(texts.some((t) => t.startsWith('堆内存:'))).toBe(true);
});

test('弹窗打开时左右切页被禁用，按 Escape/叉键可关闭弹窗', async ({ app }) => {
  await app.goto();
  await openPower(app);

  await app.tapText('重启设备');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);

  // 弹窗打开期间，按右键切页无效，仍停留在电源弹窗
  await app.pad.press('ArrowRight');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);

  // 按 Escape（叉键）关闭弹窗
  await app.pad.press('Escape');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(false);
  expect(await app.hasVisibleText('重启设备')).toBe(true);
});
