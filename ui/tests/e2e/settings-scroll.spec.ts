/**
 * 页面滚动：跟随手指的拖动、甩动的惯性、边界夹持与「滚到底不再弹回」。
 *
 * 位移取自内容列与屏幕的交集（官方 debugRect）：内容列上移多少，交集就
 * 矮多少。甩动轨迹要在松手后连续采样——越界回弹会表现为「峰值高于落点
 * 后又回落」，单调性检查正是冲着这个症状来的。
 */
import { test, expect, NAV_SETTINGS, type RemapadApp } from './fixtures';

/** 页面内容区：状态栏（26）与底栏（208 起）之间的部分。 */
const CONTENT = { x: 0, y: 34, width: 240, height: 170 };
/** 手指从 y=190 划到 y=140：位移 50 px。 */
const FINGER_TRAVEL = 50;

async function openSettings(app: RemapadApp) {
  await app.goto();
  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄配对')).toBe(true);
  await app.refreshTree();
  const content = await app.findVisibleByClass('px-4 pt-[34]');
  expect(content, '没找到设置页的滚动列').toBeDefined();
  return content!;
}

test('缓慢拖动跟随手指，松手停在原地', async ({ app }) => {
  const content = await openSettings(app);
  expect(await app.scrollOffset(content.i)).toBe(0);

  await app.touch.drag({ x: 120, y: 190 }, { x: 120, y: 150 }, { steps: 10, dwellMs: 250 });
  const samples = await app.sampleScroll(content.i, 500);
  expect(samples.length).toBeGreaterThan(4);
  const first = samples[0];
  expect(first, '按住拖动应当带动内容').toBeGreaterThan(0);
  // 界内松手：没有惯性继续滑，也没有弹回。
  expect(Math.max(...samples)).toBe(first);
  expect(Math.min(...samples)).toBe(first);
});

test('甩动到边界：越过手指位置后停在边界，不越界不回弹', async ({ app }) => {
  const content = await openSettings(app);
  await app.touch.flick({ x: 120, y: 190 }, { x: 120, y: 140 });
  const offsets = await app.sampleScroll(content.i, 1200);

  expect(offsets.length).toBeGreaterThan(6);
  const settled = offsets[offsets.length - 1];
  expect(settled, '甩动应当带着惯性越过手指位置').toBeGreaterThan(FINGER_TRAVEL);
  // 单调不减：先冲过边界再回弹会让峰值高于落点。
  for (let index = 1; index < offsets.length; index += 1) {
    expect(offsets[index], `第 ${index} 次采样回退了`).toBeGreaterThanOrEqual(
      offsets[index - 1],
    );
  }
  expect(Math.max(...offsets), '峰值必须就是落点').toBe(settled);

  // 落定之后画面不再变化（边缘弹簧回弹会在这里露出来）。
  await app.waitSettled();
  const resting = await app.regionSignature(CONTENT);
  await app.page.waitForTimeout(700);
  expect(await app.regionSignature(CONTENT)).toBe(resting);
});

test('反向甩回顶部：位移单调不增，画面与初始一致', async ({ app }) => {
  const content = await openSettings(app);
  const top = await app.regionSignature(CONTENT);

  await app.touch.flick({ x: 120, y: 190 }, { x: 120, y: 140 });
  await app.waitSettled();

  await app.touch.flick({ x: 120, y: 150 }, { x: 120, y: 200 });
  const back = await app.sampleScroll(content.i, 1200);
  expect(back[back.length - 1]).toBe(0);
  for (let index = 1; index < back.length; index += 1) {
    expect(back[index]).toBeLessThanOrEqual(back[index - 1]);
  }
  expect(Math.min(...back), '不能滑到顶部以上').toBe(0);

  await app.waitSettled();
  expect(await app.regionSignature(CONTENT), '回到顶部应当与初始画面一致').toBe(top);
});

test('首页不参与滚动：拖动不改变画面', async ({ app }) => {
  await app.goto();
  const before = await app.regionSignature(CONTENT);
  await app.touch.drag({ x: 120, y: 190 }, { x: 120, y: 130 }, { steps: 10 });
  await app.touch.flick({ x: 120, y: 190 }, { x: 120, y: 120 });
  await app.waitSettled();
  expect(await app.regionSignature(CONTENT)).toBe(before);
});
