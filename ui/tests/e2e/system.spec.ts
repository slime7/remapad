/**
 * 系统页：背光档位与重启确认框。
 *
 * 背光不提供 0 档（最低一步），且桥接协议用 0-100 传输、界面显示 1-5 档；
 * 重启走的是本应用自绘的遮罩弹窗（官方 Modal 的 portal 按 480×272 定位，
 * 在 240×280 上会错位，见 App.tsx 的注释）。
 */
import { test, expect, type RemapadApp } from './fixtures';
import { goHome, openSystem } from './pages';

/**
 * FPS 行的数值：InfoRow 里除标签外的那段文本。标签自己是 text 元素下的
 * #text 子节点，数值在同一个行容器里的另一个 text 元素中，所以从标签的
 * 父节点起逐级向上，取第一个含「另一段文本」的祖先。
 */
async function fpsText(app: RemapadApp): Promise<string> {
  const nodes = await app.nodes();
  const labelAt = nodes.findIndex((node) => node.x === 'FPS');
  if (labelAt < 0) {
    return '';
  }
  const under = (index: number, ancestor: number): boolean => {
    for (let at = index; at >= 0; at = nodes[at].p) {
      if (at === ancestor) {
        return true;
      }
    }
    return false;
  };
  for (let parent = nodes[labelAt].p; parent >= 0; parent = nodes[parent].p) {
    const value = nodes.find(
      (node, at) => at !== labelAt && node.x !== undefined && node.x !== '' && under(at, parent),
    );
    if (value !== undefined) {
      return value.x!;
    }
  }
  return '';
}

test('背光步进到 1-5 档，且不会降到 0', async ({ app }) => {
  await app.goto();
  await openSystem(app);
  // mock 出厂亮度 40 → 2 档。
  expect(await app.visibleTexts()).toContain('2');

  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);
  await app.tapText('+');
  await expect.poll(() => app.hasVisibleText('4')).toBe(true);
  await app.tapText('−');
  await expect.poll(() => app.hasVisibleText('3')).toBe(true);

  // 连点减号：档位停在 1，不会出现 0 或负档。
  for (let round = 0; round < 4; round += 1) {
    await app.tapText('−');
  }
  await expect.poll(() => app.hasVisibleText('1')).toBe(true);
  const texts = await app.visibleTexts();
  expect(texts).not.toContain('0');
});

/**
 * 弹窗里的按钮：按文本取，且必须落在弹窗子树内——「关机」既是列表行文字也是
 * 弹窗确认按钮，直接按全屏文本取会点到被遮罩盖住的那一行。
 */
async function tapDialogText(app: RemapadApp, text: string): Promise<void> {
  const nodes = await app.nodes();
  const boxIndex = nodes.findIndex((node) => node.c?.includes('bg-[#102035]') === true);
  expect(boxIndex, '没找到弹窗').toBeGreaterThanOrEqual(0);
  const under = (index: number): boolean => {
    for (let at = index; at >= 0; at = nodes[at].p) {
      if (at === boxIndex) {
        return true;
      }
    }
    return false;
  };
  const index = nodes.findIndex((node, at) => node.x === text && under(at));
  expect(index, `弹窗里没有「${text}」`).toBeGreaterThanOrEqual(0);
  await app.tapNode(nodes[index]);
}

test('关机需要确认，被外部供电拦下时给出提示', async ({ app }) => {
  await app.goto();
  await openSystem(app);

  // 取消：只关弹窗，设备继续运行。
  await app.tapText('关机');
  await expect.poll(() => app.hasVisibleText('关机？')).toBe(true);
  await tapDialogText(app, '取消');
  await expect.poll(() => app.hasVisibleText('关机？')).toBe(false);
  expect(await app.hasVisibleText('关机')).toBe(true);

  // 确认：先进入关机中；固件在 USB 供电下关不掉，回报后界面收起遮罩并提示。
  await app.tapText('关机');
  await expect.poll(() => app.hasVisibleText('关机？')).toBe(true);
  await tapDialogText(app, '关机');
  await expect.poll(() => app.hasVisibleText('关机中')).toBe(true);
  await expect
    .poll(() => app.hasVisibleText('USB 供电下无法关机，请拔线后再试'), { timeout: 10_000 })
    .toBe(true);
  await expect.poll(() => app.hasVisibleText('关机中')).toBe(false);
});

test('信息卡末行显示实时 FPS，离开系统页后停止采样', async ({ app }) => {
  await app.goto();
  await openSystem(app);

  // 进页先取一次设备时钟锚点，第二个采样窗口（约一秒）后出现实测值。
  await expect
    .poll(() => fpsText(app), { timeout: 15_000 })
    .toMatch(/^\d+(\.\d+)?$/);
  const shown = Number(await fpsText(app));

  // 同一件事的两个计数器：屏幕上按设备时钟算的帧率，应当在预览页自己数的
  // 帧率附近（两者窗口不同步，留出容差；越界说明采样换算错了）。
  const host = Number((await app.readout()).fps);
  expect(host).toBeGreaterThan(0);
  expect(Math.abs(shown - host)).toBeLessThanOrEqual(Math.max(6, host * 0.15));

  // FPS 行排在「运行时长」之后，是设备信息卡的最后一行。
  await app.refreshTree();
  const nodes = await app.nodes();
  expect(nodes.findIndex((node) => node.x === 'FPS')).toBeGreaterThan(
    nodes.findIndex((node) => node.x === '运行时长'),
  );

  // 离开系统页即停止采样：读数退回占位符，重新进页才会重新算。
  await goHome(app);
  await expect.poll(() => fpsText(app)).toBe('--');
});

test('重启需要确认，取消后弹窗关闭', async ({ app }) => {
  await app.goto();
  await openSystem(app);

  await app.tapText('重启设备');
  await expect.poll(() => app.hasVisibleText('重启设备？')).toBe(true);
  await app.refreshTree();
  const texts = await app.visibleTexts();
  expect(texts).toContain('取消');
  expect(texts).toContain('重启');

  await app.tapText('取消');
  await expect.poll(() => app.hasVisibleText('重启设备？')).toBe(false);
  // 取消只关弹窗，页面本身还在。
  expect(await app.hasVisibleText('重启设备')).toBe(true);
});
