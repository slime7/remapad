/**
 * 配对页：广播开关的状态流转，以及配对进行中对底栏的锁定。
 *
 * 配对进行中锁定底栏是产品要求（流程必须在配对页内走完），锁定失效会让
 * 用户中途切页后看不到配对进度。浏览器 mock 的时序：开始 → 扫描中 →
 * 1.5 s 后配对中 → 4.2 s 后已配对。
 */
import { test, expect, NAV_SETTINGS } from './fixtures';
import { openPairing } from './pages';

test('开始广播后进入扫描中，出现停止与 L+R 确认', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  expect(await app.hasVisibleText('未配对')).toBe(true);

  await app.tapText('开始');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);
  await app.refreshTree();
  const texts = await app.visibleTexts();
  expect(texts).toContain('停止');
  expect(texts).toContain('L+R');
  expect(texts).not.toContain('开始');
});

test('配对进行中底栏被锁定，配对完成后恢复', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await app.tapText('开始');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);

  // 进行中点底栏「设置」：不能切页，仍停在配对页。
  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await app.waitFrames(20);
  expect(await app.hasVisibleText('手柄设置'), '配对中不该切到设置页').toBe(false);
  const busyTexts = await app.visibleTexts();
  expect(
    busyTexts.includes('扫描中…') || busyTexts.includes('配对中…'),
    '配对中应当停在配对页',
  ).toBe(true);

  // mock 走完流程后解锁：这时点底栏能正常切页。
  await expect.poll(() => app.hasVisibleText('已配对'), { timeout: 15_000 }).toBe(true);
  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(true);
});
