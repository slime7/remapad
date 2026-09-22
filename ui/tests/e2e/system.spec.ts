/**
 * 系统功能测试：
 * 1. 背光调节（20%~100% 步进）；
 * 2. 电源管理（重启与关机确认弹窗）；
 * 3. 系统信息（固件版本与内存）。
 */
import { CLOVER_FACE, test, expect } from './fixtures';
import { goHome, openPower, openSystemInfo } from './pages';

test('亮度调节步进（1-5 档），最低不会降到 0 档', async ({ app }) => {
  await app.goto();
  await goHome(app);
  expect(await app.hasVisibleText('2')).toBe(true);

  // 点击加号增亮 (2档 -> 3档)
  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);

  // 点击减号减暗 (3档 -> 2档)
  await app.tapText('−');
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);

  // 连续减暗：最低停在 1 档，不会降到 0 档黑屏
  for (let i = 0; i < 4; i++) {
    await app.tapText('−');
  }
  await expect.poll(() => app.hasVisibleText('1')).toBe(true);
  expect(await app.hasVisibleText('0')).toBe(false);
});

test('亮度页加减钮是与背景瓣弧同心的 64 圆钮：右上加、右下减', async ({ app }) => {
  await app.goto();
  await goHome(app);

  // 右上瓣心（屏坐标 166,58）：钮内是 secondaryContainer 深底，距圆心 28 仍在
  // 钮内（半径 32），距圆心 38 已出钮、但仍在瓣外弧（半径 49.6）内，是四叶草浅蓝。
  expect(await app.colorAt(150, 58)).toBe('#152a1f');
  expect(await app.colorAt(166, 30)).toBe('#152a1f');
  expect(await app.colorAt(166, 20)).toBe(CLOVER_FACE);
  // 右下瓣心（屏坐标 166,150）同样成立。
  expect(await app.colorAt(150, 150)).toBe('#152a1f');
  expect(await app.colorAt(166, 188)).toBe(CLOVER_FACE);
});

test('电源管理：重启与关机斜角放在左上/右下瓣心', async ({ app }) => {
  await app.goto();
  await openPower(app);

  // 左上重启钮是 secondaryContainer 深底；右下关机钮是 errorContainer 语义色
  expect(await app.colorAt(74, 44)).toBe('#152a1f');
  expect(await app.colorAt(166, 136)).toBe('#8a1a1e');
  // 右上与左下两颗瓣心没有按钮，仍是四叶草浅蓝
  expect(await app.colorAt(166, 44)).toBe(CLOVER_FACE);
  expect(await app.colorAt(74, 164)).toBe(CLOVER_FACE);

  // 斜角布局下按钮行为不变：点击重启唤起确认弹窗，取消可关闭
  await app.tapText('重启');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);
  expect(await app.hasVisibleText('取消')).toBe(true);
  expect(await app.hasVisibleText('重启')).toBe(true);
  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(false);
});

test('电源管理：点击关机唤起确认弹窗', async ({ app }) => {
  await app.goto();
  await openPower(app);

  // 「关机」是右下 error 钮的第二行文字，点它经由祖先命中整颗按钮
  await app.tapText('关机');
  await expect.poll(() => app.hasVisibleText('是否立即关闭 Remapad 设备？')).toBe(true);
  expect(await app.hasVisibleText('关机')).toBe(true);

  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('是否立即关闭 Remapad 设备？')).toBe(false);
});

test('系统信息展示固件与内存参数', async ({ app }) => {
  await app.goto();
  await openSystemInfo(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('设备信息');
  expect(texts.some((t) => t.startsWith('固件:'))).toBe(true);
  // 内存与 PSRAM 都报「已用 / 总共」：mock 的 320 KB 堆用掉 134 KB、8 MB PSRAM 用掉 5.2 MB
  const memoryRows = async () => (await app.visibleTexts()).filter((t) => /^(堆内存|PSRAM):/.test(t));
  await expect.poll(memoryRows).toEqual(['堆内存: 134 / 320 KB', 'PSRAM: 5.2 / 8 MB']);
});

test('弹窗打开时焦点锁在弹窗内：向下键不会把焦点送到底栏连接按钮', async ({ app }) => {
  await app.goto();
  await openPower(app);

  await app.tapText('重启');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);

  // 弹窗刚打开时手柄焦点还没进来，按一次下键应落在弹窗第一颗按钮（取消）上；
  // 修复前焦连会落到底栏的连接按钮（控件树里排在弹窗前面），这里再按回车就
  // 会触发连接搜索、弹窗赖着不走。
  await app.pad.press('ArrowDown');
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(false);
  expect(await app.hasVisibleText('重启'), '关闭弹窗后电源页应仍然可见').toBe(true);
});

test('弹窗打开时左右切页被禁用，按 Escape/叉键可关闭弹窗', async ({ app }) => {
  await app.goto();
  await openPower(app);

  await app.tapText('重启');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);

  // 弹窗打开期间，按右键切页无效，仍停留在电源弹窗
  await app.pad.press('ArrowRight');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(true);

  // 按 Escape（叉键）关闭弹窗
  await app.pad.press('Escape');
  await expect.poll(() => app.hasVisibleText('是否立即重启 Remapad 设备？')).toBe(false);
  expect(await app.hasVisibleText('重启')).toBe(true);
});
