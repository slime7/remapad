/**
 * 底部状态栏测试：
 * 验证三等分状态栏、手柄操控提示与 OTA 进度条优先级展示。
 */
import { test, expect } from './fixtures';
import { openPairing } from './pages';

test('默认状态展示物理手柄、主机连接与电量读数', async ({ app }) => {
  await app.goto();
  const texts = await app.visibleTexts();
  expect(texts).toContain('未连接');
  expect(texts).toContain('88%');
});

test('手柄接入事件触发时，左区更新为已连接手柄简称', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('未连接')).toBe(true);

  // 模拟手柄连接事件
  await app.emitBridge({ t: 'padAttachedChanged', attached: true, name: 'X360' });
  await expect.poll(() => app.hasVisibleText('X360')).toBe(true);
  expect(await app.hasVisibleText('未连接')).toBe(false);

  // 模拟手柄断开
  await app.emitBridge({ t: 'padAttachedChanged', attached: false });
  await expect.poll(() => app.hasVisibleText('未连接')).toBe(true);
});

test('OTA 数据接收模式优先级最高，展示进度条', async ({ app }) => {
  await app.goto();

  // 同时开启手柄控屏与 OTA 广播，OTA 应该覆盖手柄提示
  await app.emitBridge({ t: 'padUiModeChanged', on: true });
  await app.emitBridge({ t: 'otaProgress', phase: 'receiving', percentage: 45 });

  await expect.poll(() => app.hasVisibleText('OTA 接收中: 45%')).toBe(true);
  expect(await app.hasVisibleText('翻页')).toBe(false);

  // OTA 结束回到手柄控屏提示
  await app.emitBridge({ t: 'otaProgress', phase: 'idle', percentage: 100 });
  await expect.poll(() => app.hasVisibleText('翻页')).toBe(true);
});

test('未连接状态下点击底栏中区触发连接搜索', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  expect(await app.hasVisibleText('未配对')).toBe(true);

  // 点击底栏中区（x: 120, y: 240）触发信号搜索
  await app.touch.tap(120, 240);
  await expect.poll(() => app.hasVisibleText('扫描中…') || app.hasVisibleText('连接中…')).toBe(true);
});
