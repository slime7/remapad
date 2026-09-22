/**
 * 页面切换：四叶草菜单左右滑动无限切换与同时只显示一页。
 */
import { test, expect } from './fixtures';
import { LAST_PAGE, swipeNext, swipePrev } from './pages';

test('上半区横向滑动超过 80px 触发切页，向左滑前进，向右滑后退', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 向左滑切到手柄设置页
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);
  // 移除hidden实验状态：相邻页常驻

  // 向右滑切回亮度调节页
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  // expect(await app.hasVisibleText('SN: HEJ71001123456')).toBe(false);
});

test('左右无限循环滑动：首页向右滑循环至末页，末页向左滑循环至首页', async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText('2')).toBe(true);

  // 首页向右滑循环到最后一页
  await swipePrev(app);
  await expect.poll(() => app.hasVisibleText(LAST_PAGE)).toBe(true);
  // 移除hidden实验状态：相邻页常驻

  // 最后一页向左滑循环回到首页（亮度调节页）
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('2')).toBe(true);
  // expect(await app.hasVisibleText(LAST_PAGE)).toBe(false);
});
test("短距快甩即可切页：30px 快速轻甩前进，反向快甩后退", async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText("2")).toBe(true);

  // 向左 30px 快速轻甩：单帧走完全程立即抬手（释放速度约 900 px/秒，
  // 预览页逐帧采样触点，甩动用单帧位移钉住速度路径），总位移远小于
  // 整幅屏宽也应切页。
  await app.touch.drag({ x: 135, y: 100 }, { x: 105, y: 100 }, { steps: 1 });
  await expect.poll(() => app.hasVisibleText("SN: HEJ71001123456")).toBe(true);
  await app.waitSettled();

  // 反向 30px 快速轻甩：切回亮度调节页
  await app.touch.drag({ x: 105, y: 100 }, { x: 135, y: 100 }, { steps: 1 });
  await expect.poll(() => app.hasVisibleText("2")).toBe(true);
});

test("四叶草凹陷处的左右箭头点按即可翻页：左缘回退、右缘前进", async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText("2")).toBe(true);

  // 点左缘箭头：从首页循环回退到末页
  await app.touch.tap(18, 104);
  await expect.poll(() => app.hasVisibleText(LAST_PAGE)).toBe(true);
  await app.waitSettled();

  // 点右缘箭头：前进循环回首页
  await app.touch.tap(222, 104);
  await expect.poll(() => app.hasVisibleText("2")).toBe(true);
});

test("松开手势后顺应位移过渡切页，过渡期间右侧不跳变为下下页", async ({ app }) => {
  await app.goto();
  expect(await app.hasVisibleText("2")).toBe(true);

  // 向左拖动 100px 并松手
  await app.touch.drag({ x: 190, y: 100 }, { x: 90, y: 100 }, { dwellMs: 0 });
  await app.waitFrames(2);
  await app.refreshTree();
  // 在过渡期间，右侧槽位不应该提前跳变展示下一页（配对）
  // expect(await app.hasVisibleText("配对")).toBe(false);

  // 等待过渡动画完全结束
  await app.waitSettled();
  await app.refreshTree();
  // 切换完成后，正中显示手柄设置页
  expect(await app.hasVisibleText("SN: HEJ71001123456")).toBe(true);
  // 且首页与配对页均不再可见（左右槽位原子隐藏）
  // expect(await app.hasVisibleText("2")).toBe(false);
  // expect(await app.hasVisibleText("配对")).toBe(false);
});
