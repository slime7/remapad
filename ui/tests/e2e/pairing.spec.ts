/**
 * 配对页：新链路模型下的流程语义。
 *
 * 设备上电就按凭证决定形态——从未配过任何主机时自动进入配对流程（发发现
 * 广播等主机搜索，屏幕显示「扫描中…」，主按钮变成「停止」）；配过主机则
 * 常驻唤醒广播自动回连，用户不需要按任何东西。页面上的「开始」等价于真机
 * 按住配对键：先断开当前主机再进发现广播；配对成功后固件自动退出流程。
 * 配对期间底部导航不再锁定（流程可能长期挂着等新主机）。
 *
 * 浏览器 mock 的时序：开机自动进入配对流程（扫描中 → 配对中 → 已连接，
 * 模拟主机侧 8 秒完成），按「停止」回未配对，按「开始」重新进入。
 */
import { test, expect, NAV_SETTINGS } from './fixtures';
import { openPairing } from './pages';

test('从未配过主机时，开机自动进入配对流程', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);
  await app.refreshTree();
  const texts = await app.visibleTexts();
  expect(texts).toContain('停止');
  expect(texts).toContain('L+R');
  expect(texts).not.toContain('开始');
});

test('配对键的提示文案来自界面字面量（固件回发的文本不上屏）', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  await app.tapText('停止');
  await expect.poll(() => app.hasVisibleText('已退出配对流程')).toBe(true);

  await app.tapText('开始');
  await expect.poll(() => app.hasVisibleText('广播中，等待主机连接')).toBe(true);
});

test('按停止退出配对流程，按配对重新进入', async ({ app }) => {
  await app.goto();
  await openPairing(app);

  await app.tapText('停止');
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  expect(await app.hasVisibleText('开始'), '退出后按钮回到开始').toBe(true);

  await app.tapText('开始');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);
  await app.refreshTree();
  expect(await app.visibleTexts()).toContain('停止');
});

test('配对流程中底部导航不再锁定，可以切页', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);

  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(true);
});
