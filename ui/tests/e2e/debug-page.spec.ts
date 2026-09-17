/**
 * 调试页「按键指令」区：A键、HOME/唤醒键与手柄控屏。
 */
import { test, expect } from './fixtures';
import { openDebug, openPairing } from './pages';
import { IS_DEV } from '../../src/env.generated';

test('调试页只留 A键、HOME/唤醒 与手柄控屏', async ({ app }) => {
  test.skip(!IS_DEV, '生产构建下已移除调试页');
  await app.goto();
  await openDebug(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('调试指令');
  expect(texts).toContain('A键');
  expect(texts).toContain('手柄控屏');
  expect(texts).not.toContain('配对 L+R');
});

test('HOME 按钮按主机状态换文案：离线是唤醒、在线是 HOME', async ({ app }) => {
  test.skip(!IS_DEV, '生产构建下已移除调试页');
  await app.goto();

  // 开机静默：没有主机在线，按钮按唤醒形态显示
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  await openDebug(app);
  expect(await app.visibleTexts()).toContain('唤醒');

  // 按连接把主机连上：按钮落回普通主页键 HOME
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
  await openDebug(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('HOME');
  expect(texts).not.toContain('唤醒');
});
