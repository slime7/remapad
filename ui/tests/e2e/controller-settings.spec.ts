/**
 * 手柄设置页：手柄类型切换。
 *
 * 两条历史回归点：
 *   1. 切换只改文本与 hidden，序列号行是常驻节点——重建节点会让整页卡顿
 *      （每节点实机约 50 ms），节点总数必须不变；
 *   2. 选项卡没有颜色过渡，选中色一步到位——加了 transition 之后点下去要连画
 *      150 ms 才到位，像慢半拍，所以 3 帧内就必须是终色。
 */
import { test, expect } from './fixtures';
import { openControllerSettings } from './pages';

/** 两张类型卡右侧留白处的采样点（卡内、避开图标与文字）。 */
const CARD_PRO = { x: 200, y: 83 };
const CARD_JOYCON = { x: 200, y: 144 };
const SELECTED = '#9ecefe';
const UNSELECTED = '#0c1a2c';

test('默认 Pro：只显示一条序列号', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('Pro 手柄');
  expect(texts).toContain('JoyCon 组合');
  expect(texts).toContain('序列号');
  expect(texts).toContain('HEJ71001123456');
  expect(texts).not.toContain('左序列号');
  expect(texts).not.toContain('右序列号');
  expect(await app.colorAt(CARD_PRO.x, CARD_PRO.y)).toBe(SELECTED);
  expect(await app.colorAt(CARD_JOYCON.x, CARD_JOYCON.y)).toBe(UNSELECTED);
});

test('切到 JoyCon 组合：多出左右两条序列号，节点不重建，选中色一步到位', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const nodeCount = (await app.nodes()).length;

  await app.touch.tap(CARD_JOYCON.x, CARD_JOYCON.y);
  await app.waitFrames(2);
  // 抬手后约 2 帧（33 ms）选中色就必须已经到位：加了颜色过渡时这里读到的
  // 是渐变中的中间色（实测过渡进行到一半时是 #243850 这类明显不同的值）。
  expect(await app.colorAt(CARD_JOYCON.x, CARD_JOYCON.y)).toBe(SELECTED);
  expect(await app.colorAt(CARD_PRO.x, CARD_PRO.y)).toBe(UNSELECTED);
  // 之后不再变化（过渡的中段会在这里露出来）。
  await app.waitFrames(15);
  expect(await app.colorAt(CARD_JOYCON.x, CARD_JOYCON.y)).toBe(SELECTED);
  expect(await app.colorAt(CARD_PRO.x, CARD_PRO.y)).toBe(UNSELECTED);

  await app.refreshTree();
  const texts = await app.visibleTexts();
  expect(texts).toContain('左序列号');
  expect(texts).toContain('右序列号');
  expect(texts).toContain('HBW10067012342');
  expect(texts).toContain('HCW10068012341');
  expect(texts).not.toContain('序列号');
  // 常驻节点：一次切换不该重建任何节点。
  expect((await app.nodes()).length).toBe(nodeCount);
});
