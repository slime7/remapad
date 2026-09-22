/**
 * DS4、DS5 设置页：两项开关（触摸板映射加减键、截图键）的默认状态、点按翻转
 * 与手柄操控。开关的可见状态按像素判定——滑块是亮色圆点，比轨道底色亮。
 * 开关的可点区域是 40 × 40 的命中区（轨道上下两侧的空白也算），视觉尺寸仍是
 * 36 × 20：点空白能翻转，量出来的实心像素仍是 36 × 20。
 */
import { test, expect, type FlatNode, type Rect, type RemapadApp } from './fixtures';
import { openDsSettings } from './pages';

/** 页面上的开关轨道节点（节点顺序与页面里两行的顺序一致）。 */
async function switches(app: RemapadApp): Promise<FlatNode[]> {
  const nodes = await app.nodes();
  return nodes.filter((node) => !node.hidden && node.c?.includes('w-[36] h-[20]') === true);
}

/** 开关轨道当前的矩形（静止画面也能定位，见 fixtures.locateNode）。 */
async function trackBox(app: RemapadApp): Promise<Rect> {
  const box = await app.locateNode((await switches(app))[0]);
  expect(box, '开关应当能定位到').not.toBeNull();
  return box!;
}

/**
 * 从开关中心沿一个方向数到卡片底色为止的实心像素：轨道与滑块都算，圆角两端
 * 那一档抗锯齿过渡色也算，量出来的就是开关画了多大。
 */
async function inkRun(
  app: RemapadApp,
  from: { x: number; y: number },
  delta: { x: number; y: number },
  steps: number,
  backdrop: string,
): Promise<number> {
  let count = 0;
  for (let step = 0; step <= steps; step += 1) {
    const color = await app.colorAt(from.x + delta.x * step, from.y + delta.y * step);
    if (color === backdrop) break;
    count += 1;
  }
  return count;
}

/** 颜色亮度：滑块（#d9e6ff / #a6c8ff）比轨道（#667692 / #1b416f）亮。 */
function brightness(color: string): number {
  const r = parseInt(color.slice(1, 3), 16);
  const g = parseInt(color.slice(3, 5), 16);
  const b = parseInt(color.slice(5, 7), 16);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/**
 * 滑块贴在哪一侧：在轨道中心行的左右各取一点（滑块中心离轨道中心 8px），
 * 亮的一侧就是滑块所在的一侧。取中心而不是边缘，命中框比轨道大一圈也不影响。
 */
async function knobSide(app: RemapadApp, node: FlatNode): Promise<'left' | 'right'> {
  const box = await app.locateNode(node);
  expect(box, '开关应当能定位到').not.toBeNull();
  const y = Math.round(box!.y + box!.height / 2);
  const center = Math.round(box!.x + box!.width / 2);
  const left = brightness(await app.colorAt(center - 10, y));
  const right = brightness(await app.colorAt(center + 10, y));
  return right > left ? 'right' : 'left';
}

test('DS4、DS5 设置页有两项开关：触摸板映射默认关、截图键默认开', async ({ app }) => {
  await app.goto();
  await openDsSettings(app);

  const texts = await app.visibleTexts();
  expect(texts).toContain('DS4、DS5 设置');
  expect(texts).toContain('触摸板加减');
  expect(texts).toContain('截图键');

  const rows = await switches(app);
  expect(rows, '开关应当有两个').toHaveLength(2);
  expect(await knobSide(app, rows[0])).toBe('left');
  expect(await knobSide(app, rows[1])).toBe('right');
});

test('点按开关翻转：触摸板映射打开，另一项不动', async ({ app }) => {
  await app.goto();
  await openDsSettings(app);

  const rows = await switches(app);
  await app.tapNode(rows[0]);
  await app.waitSettled();
  await app.refreshTree();

  const after = await switches(app);
  expect(await knobSide(app, after[0])).toBe('right');
  expect(await knobSide(app, after[1])).toBe('right');

  // 再点一次回到关闭：滑块回到左侧
  await app.tapNode(after[0]);
  await app.waitSettled();
  await app.refreshTree();
  expect(await knobSide(app, (await switches(app))[0])).toBe('left');
});

test('手柄操控：方向键选到开关，圆圈键翻转', async ({ app }) => {
  await app.goto();
  await openDsSettings(app);

  // 第一次下移聚焦第一个开关（触摸板映射），确认后打开
  await app.pad.press('ArrowDown');
  await app.pad.press('Enter');
  await app.waitSettled();
  await app.refreshTree();
  expect(await knobSide(app, (await switches(app))[0])).toBe('right');

  // 再下移聚焦第二个开关（截图键），确认后关闭
  await app.pad.press('ArrowDown');
  await app.pad.press('Enter');
  await app.waitSettled();
  await app.refreshTree();
  const rows = await switches(app);
  expect(await knobSide(app, rows[0])).toBe('right');
  expect(await knobSide(app, rows[1])).toBe('left');
});

test('点开关上下两侧的空白也能翻转', async ({ app }) => {
  await app.goto();
  await openDsSettings(app);

  // 轨道上方 6px：落在开关的可点区域里、轨道外
  const box = await trackBox(app);
  await app.touch.tap(box.x + box.width / 2, box.y - 6);
  await app.waitSettled();
  await app.refreshTree();

  expect(await knobSide(app, (await switches(app))[0])).toBe('right');
});

test('开关看起来还是 36 × 20', async ({ app }) => {
  await app.goto();
  await openDsSettings(app);

  const box = await trackBox(app);
  const centerX = Math.round(box.x + box.width / 2);
  const centerY = Math.round(box.y + box.height / 2);
  // 开关画在四叶草中央的浅蓝面上，取轨道上方 14px 处当底色参照
  const backdrop = await app.colorAt(centerX, box.y - 14);

  const up = await inkRun(app, { x: centerX, y: centerY }, { x: 0, y: -1 }, 20, backdrop);
  const down = await inkRun(app, { x: centerX, y: centerY }, { x: 0, y: 1 }, 20, backdrop);
  expect(up + down - 1, '轨道高').toBe(20);

  const left = await inkRun(app, { x: centerX, y: centerY }, { x: -1, y: 0 }, 30, backdrop);
  const right = await inkRun(app, { x: centerX, y: centerY }, { x: 1, y: 0 }, 30, backdrop);
  expect(left + right - 1, '轨道宽').toBe(36);
});
