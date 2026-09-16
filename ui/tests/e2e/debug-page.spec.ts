/**
 * 调试页「按键指令」区：只剩 A、HOME 与手柄操控屏幕三键——配对 L+R 已删除
 * （主机 Grip 页不再是配对入口，JoyCon 组合未配对期间由固件自动注入 L+R）。
 *
 * HOME 走实体手柄语义：主机在线时就是主页键，按钮文案是 HOME；主机睡下
 * （未连接）时按键到不了主机，按钮显示「唤醒 HOME」，点下去转成唤醒请求去
 * 打开唤醒窗口（设备平时静默，唤醒窗口是唯一的叫醒路径）。两种状态的文案
 * 都验一遍。
 */
import { test, expect } from './fixtures';
import { openDebug, openPairing } from './pages';

test('调试页不再有配对 L+R，只留 A、HOME 与手柄操控屏幕', async ({ app }) => {
  await app.goto();
  await openDebug(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('按键指令');
  expect(texts).toContain('A');
  expect(texts).toContain('手柄操控屏幕');
  expect(texts).not.toContain('配对 L+R');
  expect(texts).not.toContain('L+R');
});

test('HOME 按钮按主机状态换文案：离线是唤醒、在线是主页键', async ({ app }) => {
  await app.goto();

  // 开机静默：没有主机在线，按钮按唤醒形态显示。
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  await openDebug(app);
  expect(await app.visibleTexts()).toContain('唤醒 HOME');

  // 按连接把主机连上：按钮落回普通主页键。
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
  await openDebug(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('HOME');
  expect(texts).not.toContain('唤醒 HOME');
});
