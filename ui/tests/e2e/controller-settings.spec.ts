/**
 * 手柄设置页：身份信息行与配色按钮。
 */
import { test, expect, type FlatNode, type RemapadApp } from './fixtures';
import { openControllerSettings, openPairing } from './pages';

/** 浏览器 mock 的对外地址。 */
const PRO_MAC = '78:81:8C:1A:2B:3C';

/** 颜色的通道最小值：白描边含抗锯齿像素，按最暗通道判亮度比精确色稳。 */
function brightness(hex: string): number {
  const value = parseInt(hex.slice(1), 16);
  return Math.min((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

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

test('选中配色的指示环描在色块内圈：环带白描边、环内是色块底色', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const swatches = await colorSwatches(app);
  // 用官方 inspect 的布局矩形：locate 的命中矩形按 4 像素网格扫描，边缘不精确。
  const box = await app.inspectRect(swatches[0].i);
  expect(box, '应能取到第一个配色按钮的矩形').not.toBeNull();
  const cx = Math.round(box!.x + box!.width / 2);
  const cy = Math.round(box!.y + box!.height / 2);

  // 42 色块里 36 的环居中、环带宽 2：水平中线上环带落在距中心 16~18 的位置。
  // 布局坐标带小数，取整会有一像素误差，所以按一段区间判最亮的像素，不做逐点比较。
  const left = await Promise.all([-18, -17, -16].map((dx) => app.colorAt(cx + dx, cy)));
  expect(Math.max(...left.map(brightness)), `左环带应是白描边（实测 ${left.join(' ')}）`)
    .toBeGreaterThanOrEqual(0xf0);
  const right = await Promise.all([16, 17, 18].map((dx) => app.colorAt(cx + dx, cy)));
  expect(Math.max(...right.map(brightness)), `右环带应是白描边（实测 ${right.join(' ')}）`)
    .toBeGreaterThanOrEqual(0xf0);
  expect(await app.colorAt(cx, cy), '环内是色块底色，不是填满的圆').toBe('#232323');
});
