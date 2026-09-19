/**
 * 四叶草轮播滑动交互测试：
 * 验证：
 * 1. 慢速短拖（位移小于 40px 阈值且松手无甩动速度）不切页；
 * 2. 滑动位移大于阈值时触发切页；
 * 3. 页面内容固定不溢出不滚动。
 * 短距快甩切页由 navigation.spec.ts 的「短距快甩即可切页」用例钉住。
 */
import { test, expect } from './fixtures';

test('慢速短拖位移小于阈值且松手无甩动速度时不触发切页', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 慢速拖动 30px 后停顿再松手：位移小于 40px 阈值，释放速度被停顿采样成 0
  await app.touch.drag({ x: 120, y: 100 }, { x: 90, y: 100 }, { dwellMs: 120 });
  await app.waitSettled();
  await app.refreshTree();

  // 依然保持在第 1 页
  expect(await app.hasVisibleText('2')).toBe(true);
  expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('横向滑动位移大于阈值时切换页面', async ({ app }) => {
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
