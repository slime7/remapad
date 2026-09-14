/**
 * 手柄设置页：手柄类型切换与身份信息行。
 *
 * 三条历史回归点：
 *   1. 切换只改文本与 hidden，四条信息行是常驻节点——重建节点会让整页卡顿
 *      （每节点实机约 50 ms），节点总数必须不变；
 *   2. 选项卡没有颜色过渡，选中色一步到位——加了 transition 之后点下去要连画
 *      150 ms 才到位，像慢半拍，所以 3 帧内就必须是终色；
 *   3. 信息行是「标签 值」单节点：Pro 两行（序列号、MAC），JoyCon 四行
 *      （左序列号、左 MAC、右序列号、右 MAC），地址取固件应答。
 */
import { test, expect } from './fixtures';
import { openControllerSettings } from './pages';

/** 两张类型卡右侧留白处的采样点（卡内、避开图标与文字；顶部无标签行，
 *  首卡从 pt-[34] 起，卡高 53、卡距 8）。 */
const CARD_PRO = { x: 200, y: 60 };
const CARD_JOYCON = { x: 200, y: 121 };
const SELECTED = '#9ecefe';
const UNSELECTED = '#0c1a2c';

/** 浏览器 mock 的对外地址，与固件派生规则同形（见 ui/src/bridge/mock.ts）。 */
const PRO_MAC = '78:81:8C:1A:2B:3C';
const LEFT_MAC = 'E9:D4:62:0F:14:48';
const RIGHT_MAC = 'CA:8A:D9:29:23:6F';

test('默认 Pro：序列号与 MAC 两行，右只两行收起', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const texts = await app.visibleTexts();
  expect(texts).toContain('Pro 手柄');
  expect(texts).toContain('JoyCon 组合');
  expect(texts).toContain('序列号 HEJ71001123456');
  expect(texts).toContain(`MAC ${PRO_MAC}`);
  expect(texts).not.toContain('左序列号 HBW10067012342');
  expect(texts).not.toContain('右序列号 HCW10068012341');
  expect(texts).not.toContain(`右 MAC ${RIGHT_MAC}`);
  expect(await app.colorAt(CARD_PRO.x, CARD_PRO.y)).toBe(SELECTED);
  expect(await app.colorAt(CARD_JOYCON.x, CARD_JOYCON.y)).toBe(UNSELECTED);
});

test('切到 JoyCon 组合：左右各两条信息行，节点不重建，选中色一步到位', async ({ app }) => {
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
  expect(texts).toContain('左序列号 HBW10067012342');
  expect(texts).toContain(`左 MAC ${LEFT_MAC}`);
  expect(texts).toContain('右序列号 HCW10068012341');
  expect(texts).toContain(`右 MAC ${RIGHT_MAC}`);
  expect(texts).not.toContain('序列号 HEJ71001123456');
  expect(texts).not.toContain(`MAC ${PRO_MAC}`);
  // 常驻节点：一次切换不该重建任何节点。
  expect((await app.nodes()).length).toBe(nodeCount);
});
