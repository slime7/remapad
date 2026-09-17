/**
 * 手柄操控模式测试：
 * 契约：
 * 1. 左右方向键切换上方页面；
 * 2. 上下方向键在当前页面的交互控件间切换；
 * 3. 回车/空格确认激活聚焦控件；
 * 4. padUiMode 开启时底部展示手柄操作提示。
 */
import { test, expect } from './fixtures';

test('方向键左右切换四叶草页面', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 按右键切换到第 2 页：手柄设置
  await app.pad.press('ArrowRight');
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);
  expect(await app.hasVisibleText('2')).toBe(false);

  // 按左键切换回第 1 页：亮度调节
  await app.pad.press('ArrowLeft');
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('上下方向键选择控件并回车激活：在亮度页调节背光', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 第一次下移聚焦第 1 个按钮（加号），回车激活增加亮度 (2档 -> 3档)
  await app.pad.press('ArrowDown');
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);

  // 第二次下移聚焦第 2 个按钮（减号），回车激活降低亮度 (3档 -> 2档)
  await app.pad.press('ArrowDown');
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
});

test('WASD 与方向键等价', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // KeyD（右）切到手柄设置页
  await app.pad.press('KeyD');
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);

  // KeyA（左）切回亮度调节页
  await app.pad.press('KeyA');
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
});

test('手柄控屏模式激活时，底栏切换为两行按键提示文本', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('未连接')).toBe(true);

  // 模拟手柄控屏模式广播
  await app.emitBridge({ t: 'padUiModeChanged', on: true });
  await expect.poll(() => app.hasVisibleText('翻页')).toBe(true);
  expect(await app.hasVisibleText('选择')).toBe(true);
  expect(await app.hasVisibleText('确认')).toBe(true);
  expect(await app.hasVisibleText('长按')).toBe(true);
  expect(await app.hasVisibleText('退出')).toBe(true);

  // 退出控屏模式，恢复状态显示
  await app.emitBridge({ t: 'padUiModeChanged', on: false });
  await expect.poll(() => app.hasVisibleText('未连接')).toBe(true);
  expect(await app.hasVisibleText('翻页')).toBe(false);
});
