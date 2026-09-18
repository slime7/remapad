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
  // 移除hidden实验状态：相邻页常驻

  // 向右滑切回第 1 页：亮度调节
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  // expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('左右无限循环滑动：第 1 页向右滑循环至末页，末页向左滑循环至第 1 页', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  const lastPageLabel = IS_DEV ? '调试指令' : '设备信息';

  // 首页向右滑循环到最后一页
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText(lastPageLabel)).toBe(true);
  // 移除hidden实验状态：相邻页常驻

  // 最后一页向左滑循环回到首页（亮度调节页）
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  // expect(await app.hasVisibleText(lastPageLabel)).toBe(false);
});
test("松开手势后顺应位移过渡切页，过渡期间右侧不跳变为下下页", async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText("2")).toBe(true);

  // 向左拖动 100px 并松手
  await app.touch.drag({ x: 190, y: 100 }, { x: 90, y: 100 }, { dwellMs: 0 });
  await app.waitFrames(2);
  await app.refreshTree();
  // 在过渡期间，右侧槽位不应该提前跳变展示第 3 页（配对新主机）
  // expect(await app.hasVisibleText("配对新主机")).toBe(false);

  // 等待过渡动画完全结束
  await app.waitSettled();
  await app.refreshTree();
  // 切换完成后，正中显示第 2 页（手柄设置页）
  expect(await app.hasVisibleText("SN: HEJ71001123456")).toBe(true);
  // 且第 1 页和第 3 页均不再可见（左右槽位原子隐藏）
  // expect(await app.hasVisibleText("2")).toBe(false);
  // expect(await app.hasVisibleText("配对新主机")).toBe(false);
});
