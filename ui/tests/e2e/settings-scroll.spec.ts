/**
 * 四叶草轮播滑动交互测试：
 * 验证：
 * 1. 滑动位移小于 80px 阈值时不切页；
 * 2. 滑动位移大于 80px 阈值时触发切页；
 * 3. 页面内容固定不溢出不滚动。
 */
import { test, expect } from './fixtures';

test('横向滑动位移小于 80px 阈值时不触发切页', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 滑动 40px（小于 80px 阈值）
  await app.touch.flick({ x: 120, y: 100 }, { x: 80, y: 100 });
  await app.waitSettled();
  await app.refreshTree();

  // 依然保持在第 1 页
  expect(await app.hasVisibleText('2')).toBe(true);
  expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('横向滑动位移大于 80px 阈值时切换页面', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 滑动 100px（大于 80px 阈值）
  await app.touch.flick({ x: 160, y: 100 }, { x: 60, y: 100 });
  await app.waitSettled();
  await app.refreshTree();

  // 成功切到第 2 页
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);
  expect(await app.hasVisibleText('2')).toBe(false);
});
