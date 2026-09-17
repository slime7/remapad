/**
 * 手柄设置页：身份信息行与配色按钮。
 */
import { test, expect, type FlatNode, type RemapadApp } from './fixtures';
import { openControllerSettings, openPairing } from './pages';

/** 浏览器 mock 的对外地址。 */
const PRO_MAC = '78:81:8C:1A:2B:3C';

/** 页面上的配色按钮（节点顺序与页面里的 COLORWAYS 一致）。 */
async function colorSwatches(app: RemapadApp): Promise<FlatNode[]> {
  const nodes = await app.nodes();
  return nodes.filter(
    (node) => !node.hidden && node.c?.includes('w-[42] h-[42]') === true,
  );
}

test('手柄设置页只呈现 Pro：序列号与 MAC 两行', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('SN: HEJ71001123456');
  expect(texts).toContain(`MAC: ${PRO_MAC}`);
  expect(texts).not.toContain('JoyCon 组合');
});

test('配色按钮有四个，点另一款后选中状态切换', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const swatches = await colorSwatches(app);
  expect(swatches, '配色按钮应当有四个').toHaveLength(4);

  // 点选第二款配色
  await app.tapNode(swatches[1]);
  await app.waitSettled();
});

test('配对后再点配色：设备自动回连', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);

  await openControllerSettings(app);
  const swatches = await colorSwatches(app);
  await app.tapNode(swatches[1]);
  await app.waitSettled();

  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
});
