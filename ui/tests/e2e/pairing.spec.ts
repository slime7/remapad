/**
 * 配对页：连接键（开广播）与配对副按钮。
 */
import { CLOVER_FACE, test, expect } from './fixtures';
import { openPairing, swipeNext, swipePrev } from './pages';

test('开机静默：未配对也不广播，主按钮是「连接」', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  const texts = await app.visibleTexts();
  expect(texts).toContain('连接');
  expect(texts).toContain('配对');
  expect(texts, '没有窗口就不该在广播').not.toContain('扫描中…');
  expect(texts).not.toContain('停止');
});

test('配对页提示在上方，两枚圆钮落在下方两角的瓣心', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);

  // 右下主钮与左下副钮的钮内采样点都是 secondaryContainer 深底，
  // 上方两颗瓣心没有按钮，仍是四叶草浅蓝。
  expect(await app.colorAt(166, 136)).toBe('#152a1f');
  expect(await app.colorAt(74, 136)).toBe('#152a1f');
  expect(await app.colorAt(74, 44)).toBe(CLOVER_FACE);
  expect(await app.colorAt(166, 44)).toBe(CLOVER_FACE);

  // 提示文字块在页面上部（屏坐标 y < 80），主按钮文字在下半区（y > 100）。
  // 静止画面下官方 inspect 拿不到静态文本的矩形，用边界命中扫描定位。
  const status = await app.findByText('未配对');
  expect(status).toBeDefined();
  const statusRect = await app.locateNode(status!);
  expect(statusRect).not.toBeNull();
  expect(statusRect!.y).toBeLessThan(80);
  const main = await app.findByText('连接');
  expect(main).toBeDefined();
  const mainRect = await app.locateNode(main!);
  expect(mainRect).not.toBeNull();
  expect(mainRect!.y).toBeGreaterThan(100);
});

test('配对键的提示文案来自界面字面量（固件回发的文本不上屏）', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  // 副按钮配新主机：断开当前主机后进发现广播。
  await app.tapText('配对');
  await expect.poll(() => app.hasVisibleText('广播中，等待主机连接')).toBe(true);

  // 连接键在静默时打开连接窗口，提示同样来自界面字面量。
  await app.tapText('停止');
  await expect.poll(() => app.hasVisibleText('已停止广播')).toBe(true);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已打开连接，等待主机连回来')).toBe(true);
});

test('按连接开广播等主机，按停止回到静默', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);
  await app.refreshTree();
  expect(await app.visibleTexts()).toContain('停止');

  await app.tapText('停止');
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  expect(await app.hasVisibleText('连接'), '停止广播后按钮回到连接').toBe(true);
  expect(await app.hasVisibleText('扫描中…')).toBe(false);
});

test('已配对后按断开回到静默，按连接重新连上', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  // 未配对时按连接走配对流程，主机配上后凭证落到这一身份。
  await app.tapText('连接');
  // 连接中会展示 Braille spinner 动画字符
  await expect.poll(async () => {
    const texts = await app.visibleTexts();
    return texts.some((t) => ['⠁', '⠂', '⠄', '⡀', '⢀', '⠠', '⠐', '⠈'].includes(t));
  }, { timeout: 10_000 }).toBe(true);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);

  // 连上主机后 spinner 应当隐藏，不再显示任何 loading 字符
  const connectedTexts = await app.visibleTexts();
  expect(connectedTexts.some((t) => ['⠁', '⠂', '⠄', '⡀', '⢀', '⠠', '⠐', '⠈'].includes(t))).toBe(false);

  // 断开：链路放下、广播收掉，凭证还在，屏幕回到「已配对」静默态。
  await app.tapText('断开');
  await expect.poll(() => app.hasVisibleText('已配对')).toBe(true);
  expect(await app.hasVisibleText('连接'), '断开后按钮回到连接').toBe(true);

  // 再按连接：已配对身份发回连形态，主机连回来。
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
});

test('配对流程中可自由滑动切页', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);

  // 向右滑离开配对页：配对页文案不再可见，切回来页面还在（广播流程与页面无关）
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText('配对')).toBe(false);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('配对')).toBe(true);
});
