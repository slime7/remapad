/**
 * 配对页：连接键（开广播）与配对新主机。
 *
 * 设备与真机一样不主动发信号：开机静默（未配对显示「未配对」、配过主机显示
 * 「已配对」），主机睡下断开后也不再广播。主按钮是连接键——未连接时按它开
 * 连接窗口（未配对进配对流程等主机搜，已配对发回连形态等主机连回来），广播
 * 中变「停止」，链路在线时变「断开」。副按钮配新主机：断开当前主机再进发现
 * 广播，配上由固件自动退出流程。
 *
 * 浏览器 mock 的时序：开机静默；未配对按连接 → 扫描中 → 配对中 → 已连接
 * （模拟主机侧 8 秒完成）；已配对按连接 → 连接中 → 已连接（2 秒回连）。
 */
import { test, expect, NAV_SETTINGS } from './fixtures';
import { openPairing } from './pages';

test('开机静默：未配对也不广播，主按钮是「连接」', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  const texts = await app.visibleTexts();
  expect(texts).toContain('连接');
  expect(texts).toContain('新主机');
  expect(texts, '没有窗口就不该在广播').not.toContain('扫描中…');
  expect(texts).not.toContain('停止');
});

test('配对键的提示文案来自界面字面量（固件回发的文本不上屏）', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  // 副按钮配新主机：断开当前主机后进发现广播。
  await app.tapText('新主机');
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
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);

  // 断开：链路放下、广播收掉，凭证还在，屏幕回到「已配对」静默态。
  await app.tapText('断开');
  await expect.poll(() => app.hasVisibleText('已配对')).toBe(true);
  await expect.poll(() => app.hasVisibleText('已断开连接')).toBe(true);
  expect(await app.hasVisibleText('连接'), '断开后按钮回到连接').toBe(true);

  // 再按连接：已配对身份发回连形态，主机连回来。
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('连接中…')).toBe(true);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 15_000 }).toBe(true);
});

test('配对流程中底部导航不再锁定，可以切页', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);

  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(true);
});
