/**
 * 启动与首屏：应用首帧就绪后的可见事实。
 * 验证四叶草菜单首页（亮度）与底部状态卡片。
 */
import { test, expect } from './fixtures';

test('首帧就绪，首页与底部状态栏都画出来了', async ({ app }) => {
  await app.goto();
  const readout = await app.readout();
  expect(readout.status).toContain('运行中');
  expect(readout.log).toContain('已加载 remapad-ui');
  expect(app.consoleLines.filter((line) => line.includes('[pageerror]'))).toEqual([]);
  expect((await app.appConsole()).filter((line) => line.level === 'error')).toEqual([]);

  // 底部状态栏显示手柄未连接与电量读数
  const texts = await app.visibleTexts();
  expect(texts).toContain('未连接');
  expect(texts).toContain('88%');

  // 第一页为亮度调节页，默认背光 40% 对应 2 档
  expect(texts).toContain('2');
});

test('页面在首屏前一次挂完，未激活页面处于 hidden', async ({ app }) => {
  await app.goto();
  const texts = await app.visibleTexts();
  // 当前处于第 1 页亮度调节，未激活页面的特有文案不应可见
  expect(texts).toContain('2');
  expect(await app.hasVisibleText('重启设备')).toBe(false);
  expect(await app.hasVisibleText('调试指令')).toBe(false);
});
