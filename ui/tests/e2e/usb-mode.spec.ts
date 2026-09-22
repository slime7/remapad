/**
 * USB 模式页：串口 / 手柄两档。切到「手柄」先弹确认（PC 上的串口会消失），
 * 切回「串口」后询问是否立刻重启设备。
 * 选中态按像素判定：选中卡是 onPrimaryContainer 深蓝底、未选中卡是比四叶草
 * 浅蓝面更亮的浅底，两态都不与面色重合（否则选中的那张会看不出来）。
 */
import { test, expect, type RemapadApp } from './fixtures';
import { openUsbMode } from './pages';

/** 选项卡现在的底色：在卡片右侧空白处取样（图标与文字都在左侧）。 */
async function cardColor(app: RemapadApp, title: string): Promise<string> {
  const node = await app.findByText(title);
  expect(node, `没找到选项卡「${title}」`).toBeDefined();
  const box = await app.locateNode(node!);
  expect(box, `选项卡「${title}」当前点不到`).not.toBeNull();
  const x = Math.round(box!.x + box!.width - 20);
  const y = Math.round(box!.y + box!.height / 2);
  return app.colorAt(x, y);
}

test('USB 模式页两档都画出来，默认选中串口', async ({ app }) => {
  await app.goto();
  await openUsbMode(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('USB 模式');
  expect(texts).toContain('串口');
  expect(texts).toContain('PC 桥接与烧录');
  expect(texts).toContain('手柄');
  expect(texts).toContain('直插手柄输入');

  // 开机默认角色是串口：串口卡深蓝底（选中），手柄卡浅底
  expect(await cardColor(app, '串口')).toBe('#1b416f');
  expect(await cardColor(app, '手柄')).toBe('#d9e6ff');
});

test('切到「手柄」先弹确认，取消不改角色', async ({ app }) => {
  await app.goto();
  await openUsbMode(app);

  await app.tapText('手柄');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(true);
  expect(await app.hasVisibleText('只能在本屏幕切回或重启设备')).toBe(true);

  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(false);
  expect(await cardColor(app, '串口')).toBe('#1b416f');
});

test('模式页不含底部提示行：两张卡都完整落在中央内容框内', async ({ app }) => {
  await app.goto();
  await openUsbMode(app);

  // host 档是提示行唯一会出现的状态（切过去之前由 App 弹确认）
  await app.tapText('手柄');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(true);
  await app.tapText('切换');
  await expect.poll(() => cardColor(app, '手柄')).toBe('#1b416f');

  // 中央内容框是 x 50..206、y 54..202（overflow-hidden）：卡完整在内才不会被框沿裁掉
  for (const title of ['串口', '手柄']) {
    const node = await app.findByText(title);
    expect(node, `没找到选项卡「${title}」`).toBeDefined();
    const box = await app.locateNode(node!);
    expect(box, `选项卡「${title}」当前点不到`).not.toBeNull();
    expect(box!.y, `选项卡「${title}」顶到内容框上沿`).toBeGreaterThanOrEqual(54);
    expect(box!.y + box!.height, `选项卡「${title}」压到内容框下沿`).toBeLessThanOrEqual(202);
  }
});

test('确认后角色切到手柄：选中卡换到「手柄」', async ({ app }) => {
  await app.goto();
  await openUsbMode(app);

  await app.tapText('手柄');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(true);
  await app.tapText('切换');
  await expect.poll(() => cardColor(app, '手柄')).toBe('#1b416f');
  expect(await cardColor(app, '串口')).toBe('#d9e6ff');
  // 切到 host 不涉及重启询问
  expect(await app.hasVisibleText('已切回串口')).toBe(false);
});

test('切回串口后询问是否立刻重启：稍后留在本页，立即重启进重启遮罩', async ({ app }) => {
  await app.goto();
  await openUsbMode(app);

  await app.tapText('手柄');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(true);
  await app.tapText('切换');
  await expect.poll(() => cardColor(app, '手柄')).toBe('#1b416f');

  // 切回串口是恢复方向：不弹确认，切完问要不要立刻重启
  await app.tapText('串口');
  await expect.poll(() => app.hasVisibleText('已切回串口')).toBe(true);
  expect(await app.hasVisibleText('是否立即重启设备？')).toBe(true);

  // 稍后：弹窗收起，页面回到串口选中态
  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('已切回串口')).toBe(false);
  expect(await cardColor(app, '串口')).toBe('#1b416f');

  // 再切一次并立即重启：进重启遮罩
  await app.tapText('手柄');
  await expect.poll(() => app.hasVisibleText('切换到手柄模式')).toBe(true);
  await app.tapText('切换');
  await expect.poll(() => cardColor(app, '手柄')).toBe('#1b416f');
  await app.tapText('串口');
  await expect.poll(() => app.hasVisibleText('已切回串口')).toBe(true);
  await app.tapText('立即重启');
  await expect.poll(() => app.hasVisibleText('重启中…')).toBe(true);
});
