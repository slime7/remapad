/**
 * 系统页：背光档位与重启确认框。
 *
 * 背光不提供 0 档（最低一步），且桥接协议用 0-100 传输、界面显示 1-5 档；
 * 重启走的是本应用自绘的遮罩弹窗（官方 Modal 的 portal 按 480×272 定位，
 * 在 240×280 上会错位，见 App.tsx 的注释）。
 */
import { test, expect } from './fixtures';
import { openSystem } from './pages';

test('背光步进到 1-5 档，且不会降到 0', async ({ app }) => {
  await app.goto();
  await openSystem(app);
  // mock 出厂亮度 40 → 2 档。
  expect(await app.visibleTexts()).toContain('2');

  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);
  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('4')).toBe(true);
  await app.tapText('−');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);

  // 连点减号：档位停在 1，不会出现 0 或负档。
  for (let round = 0; round < 4; round += 1) {
    await app.tapText('−');
  }
  await expect.poll(() => app.hasVisibleText('1')).toBe(true);
  const texts = await app.visibleTexts();
  expect(texts).not.toContain('0');
});

test('重启需要确认，取消后弹窗关闭', async ({ app }) => {
  await app.goto();
  await openSystem(app);

  await app.tapText('重启设备');
  await expect.poll(() => app.hasVisibleText('重启设备？')).toBe(true);
  await app.refreshTree();
  const texts = await app.visibleTexts();
  expect(texts).toContain('取消');
  expect(texts).toContain('重启');

  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('重启设备？')).toBe(false);
  // 取消只关弹窗，页面本身还在。
  expect(await app.hasVisibleText('重启设备')).toBe(true);
});
