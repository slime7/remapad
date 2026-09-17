/**
 * 页面切换：四叶草菜单左右滑动无限切换与同时只显示一页。
 */
import { test, expect } from './fixtures';
import { swipeNext, swipePrev } from './pages';
import { IS_DEV } from '../../src/env.generated';

test('上半区横向滑动超过 80px 触发切页，向左滑前进，向右滑后退', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 向左滑切到第 2 页：手柄设置
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);
  expect(await app.hasVisibleText('2')).toBe(false);

  // 向右滑切回第 1 页：亮度调节
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('左右无限循环滑动：第 1 页向右滑循环至末页，末页向左滑循环至第 1 页', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  const lastPageLabel = IS_DEV ? '调试指令' : '设备信息';

  // 首页向右滑循环到最后一页
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText(lastPageLabel)).toBe(true);
  expect(await app.hasVisibleText('2')).toBe(false);

  // 最后一页向左滑循环回到首页（亮度调节页）
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  expect(await app.hasVisibleText(lastPageLabel)).toBe(false);
});
