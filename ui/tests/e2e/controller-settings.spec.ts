/**
 * 手柄设置页：身份信息行与配色按钮。
 *
 * 三个回归点：
 *   1. 信息行是「标签 值」单节点，地址取固件应答——页面只重写文本；
 *   2. 设备对外只呈现 Pro Controller 2：页面没有手柄形态选择，也不出现
 *      第二种手柄的信息行（JoyCon 形态已从产品里移除）；
 *   3. 配色按钮一行四个、等距、48 尺寸，点一下选中项就跟过去（就是把整组
 *      四段颜色写给了固件）。
 */
import { test, expect, type FlatNode, type Rect, type RemapadApp } from './fixtures';
import { openControllerSettings, openPairing } from './pages';

/** 浏览器 mock 的对外地址，与固件派生规则同形（见 ui/src/bridge/mock.ts）。 */
const PRO_MAC = '78:81:8C:1A:2B:3C';

/** 深机身上的选中环颜色（onSurface）与浅机身上的选中环颜色（页面底色）。 */
const RING_ON_DARK = '#d9e6ff';
const RING_ON_LIGHT = '#060f1b';

/** 环带上的取样点：48 圆钮内的 2px 环画在直径 40 的圆上（见 theme 的 colorSwatchRing）。 */
const ringPoint = (rect: Rect): { x: number; y: number } => ({
  x: rect.x + 24,
  y: rect.y + 24 - 20,
});

/** 页面上的配色按钮（节点顺序与页面里的 COLORWAYS 一致）。 */
async function colorSwatches(app: RemapadApp): Promise<FlatNode[]> {
  const nodes = await app.nodes();
  return nodes.filter(
    (node) => !node.hidden && node.c?.includes('w-[48] h-[48]') === true,
  );
}

/** 逐个取矩形的排版信息（尺寸与间距）。 */
async function swatchRects(app: RemapadApp, swatches: FlatNode[]): Promise<Rect[]> {
  const rects: Rect[] = [];
  for (const swatch of swatches) {
    const rect = await app.inspectRect(swatch.i);
    expect(rect, '配色按钮应当有矩形信息').not.toBeNull();
    rects.push(rect!);
  }
  return rects;
}

test('手柄设置页只呈现 Pro：序列号与 MAC 两行，没有形态选择与第二台手柄的行', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('序列号 HEJ71001123456');
  expect(texts).toContain(`MAC ${PRO_MAC}`);
  // 形态切换卡与第二台手柄的专属信息行都不再出现在产品里。
  expect(texts).not.toContain('Pro 手柄');
  expect(texts).not.toContain('JoyCon 组合');
  expect(texts).not.toContain('左序列号 HBW10067012342');
  expect(texts).not.toContain('左 MAC E9:D4:62:0F:14:48');
  expect(texts).not.toContain('右序列号 HCW10068012341');
  expect(texts).not.toContain('右 MAC CA:8A:D9:29:23:6F');
});

test('配色按钮一行四个等距 48 尺寸，点另一款后选中环跟过去', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const swatches = await colorSwatches(app);
  expect(swatches, '配色按钮应当有四个').toHaveLength(4);

  const rects = await swatchRects(app, swatches);
  // 48 尺寸的圆钮，一排铺满列宽：四个间距相同。
  expect([rects[0].width, rects[0].height]).toEqual([48, 48]);
  const gaps = rects
    .slice(1)
    .map((rect, index) => Math.round(rect.x - (rects[index].x + rects[index].width)));
  expect(Math.max(...gaps) - Math.min(...gaps), '四个按钮的间距应当相同（±1px 取整）')
    .toBeLessThanOrEqual(1);
  expect(Math.abs(rects[0].y - rects[3].y), '四个按钮在同一行').toBeLessThanOrEqual(1);

  const ringAt = async (rect: Rect) => app.colorAt(ringPoint(rect).x, ringPoint(rect).y);

  // 默认选中第一款（标准黑）：深机身用浅色环，未选中的三款环与机身同色（看不见）。
  expect(await ringAt(rects[0])).toBe(RING_ON_DARK);
  expect(await ringAt(rects[1])).toBe('#3a4045');
  expect(await ringAt(rects[2])).toBe('#b9bec4');
  expect(await ringAt(rects[3])).toBe('#1e3b2a');

  // 点第三款（银灰）：浅机身换成深色环才看得见，第一款恢复成未选中。
  await app.tapNode(swatches[2]);
  await app.waitSettled();
  await expect.poll(() => ringAt(rects[2])).toBe(RING_ON_LIGHT);
  expect(await ringAt(rects[0])).toBe('#232323');
});

test('配对后再点配色：设备自动回连，屏幕回到已连接', async ({ app }) => {
  await app.goto();
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接')).toBe(true);

  /* 换配色等价于换一只手柄上电：主机读到的就是新颜色，设备自己把连接窗口
     打开让主机连回来——用户不用再按一次连接键。 */
  await openControllerSettings(app);
  const swatches = await colorSwatches(app);
  await app.tapNode(swatches[1]);
  await app.waitSettled();

  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('已连接')).toBe(true);
});
