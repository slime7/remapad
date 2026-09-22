/**
 * 底部状态栏测试：
 * 左区是 USB 模式指示（串口→电脑图标、手柄→手柄图标；PC 没接上、手柄没插上
 * 时换成同族的禁用字形、颜色降一档、标签写「未连接」），中区是主机连接，右区是电量；
 * 另验证手柄操控提示与 OTA 进度条的优先级展示。
 */
import { test, expect } from './fixtures';
import { openPairing } from './pages';
import { ICON } from '../../src/iconGlyphs';

test('默认状态左区是串口档的电脑图标，右区是电量读数', async ({ app }) => {
  await app.goto();
  const texts = await app.visibleTexts();
  expect(texts).toContain('PC');
  expect(texts).toContain(ICON.desktopWindows);
  expect(texts).toContain('88%');
});

test('串口档下拔掉 PC：换成同一台电脑的禁用字形与「未连接」', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText(ICON.desktopWindows)).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopAccessDisabled)).toBe(false);

  await app.emitBridge({ t: 'pcLinkChanged', connected: false });
  await expect.poll(() => app.hasVisibleText('未连接')).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopAccessDisabled)).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopWindows)).toBe(false);

  // 插回 PC：图标与标签都回到接入形态
  await app.emitBridge({ t: 'pcLinkChanged', connected: true });
  await expect.poll(() => app.hasVisibleText('PC')).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopWindows)).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopAccessDisabled)).toBe(false);
});

test('切到手柄档：图标换成手柄，插上手柄显示家族简称', async ({ app }) => {
  await app.goto();
  await app.emitBridge({ t: 'usbRoleChanged', role: 'host', active: true });

  // 没插手柄：禁用形态的图标 + 未连接
  await expect.poll(() => app.hasVisibleText('未连接')).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAssetOff)).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAsset)).toBe(false);

  // 插上手柄：固件报家族机读 token，屏幕标签取自 ui/src 的字面量
  await app.emitBridge({ t: 'padAttachedChanged', attached: true, name: 'xbox' });
  await expect.poll(() => app.hasVisibleText('XBOX')).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAsset)).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAssetOff)).toBe(false);

  // 拔掉手柄回到禁用形态
  await app.emitBridge({ t: 'padAttachedChanged', attached: false });
  await expect.poll(() => app.hasVisibleText('未连接')).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAssetOff)).toBe(true);
});

test('模式切回串口：左区立刻回到电脑图标与 PC 标签', async ({ app }) => {
  await app.goto();
  await app.emitBridge({ t: 'usbRoleChanged', role: 'host', active: true });
  await app.emitBridge({ t: 'padAttachedChanged', attached: true, name: 'ps' });
  await expect.poll(() => app.hasVisibleText('PS')).toBe(true);

  await app.emitBridge({ t: 'usbRoleChanged', role: 'device', active: true });
  await expect.poll(() => app.hasVisibleText('PC')).toBe(true);
  expect(await app.hasVisibleText(ICON.desktopWindows)).toBe(true);
  expect(await app.hasVisibleText(ICON.videogameAsset)).toBe(false);
});

test('处于串口档时手柄接入事件不改左区（图标跟模式走）', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('PC')).toBe(true);

  await app.emitBridge({ t: 'padAttachedChanged', attached: true, name: 'X360' });
  await expect.poll(() => app.hasVisibleText('PC')).toBe(true);
  expect(await app.hasVisibleText('X360')).toBe(false);
  expect(await app.hasVisibleText(ICON.desktopWindows)).toBe(true);
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

test('未连接主机时点击底栏中区触发连接搜索', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  expect(await app.hasVisibleText('未配对')).toBe(true);

  // 点击底栏中区（x: 120, y: 240）触发信号搜索
  await app.touch.tap(120, 240);
  await expect.poll(() => app.hasVisibleText('扫描中…') || app.hasVisibleText('连接中…')).toBe(true);
});
