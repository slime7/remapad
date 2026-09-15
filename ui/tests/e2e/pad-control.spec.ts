/**
 * 手柄操控屏幕：方向键移动焦点、圆圈键确认。
 *
 * 真机上这两件事由固件的手柄操控模式驱动（组合键把输入从主机收给屏幕，
 * 见 docs/adr/0028）；浏览器预览里键盘随时可用，按键位经预览页送进同一套
 * frame 契约，所以「按得对、画得出」与真机是同一条聚焦与激活通路。
 */
import { test, expect, NAV_BAR, type RemapadApp } from './fixtures';
import { openSettings } from './pages';

/** 底栏右侧「设置」键（left/right/bottom 各 8，键宽 64）：取整块，环画在边界上。 */
const SETTINGS_KEY = { x: 168, y: 208, width: 64, height: 64 };

/** 焦点环专用亮度阈值：界面文字最亮的通道是 217（onSurface #d9e6ff），环是纯白。 */
const RING_LEVEL = 240;

/**
 * 在 [y0, y1) 里逐条带找焦点环，返回有环的条带起点。环被上层悬浮节点盖住时
 * 就画不出来——这正是「选中项被底栏挡住」的判据。整段一次取回，轮询才够快。
 */
async function ringBands(app: RemapadApp, y0: number, y1: number): Promise<number[]> {
  return app.page.evaluate(
    ({ top, bottom, level }) => {
      const canvas = document.getElementById('screen') as HTMLCanvasElement;
      const context = canvas.getContext('2d')!;
      const bands: number[] = [];
      for (let y = top; y < bottom; y += 4) {
        const data = context.getImageData(0, y, 240, 4).data;
        let hits = 0;
        for (let index = 0; index < data.length; index += 4) {
          if (data[index] >= level && data[index + 1] >= level && data[index + 2] >= level) {
            hits += 1;
          }
        }
        if (hits / (data.length / 4) > 0.004) {
          bands.push(y);
        }
      }
      return bands;
    },
    { top: y0, bottom: y1, level: RING_LEVEL },
  );
}

/** 首页左侧状态圆（56 见方，行内居中，pt-[38]）：USB 链路。 */
const USB_CIRCLE = { x: 60, y: 38, width: 56, height: 56 };
/** 页面内容区（避开每秒刷新的状态栏与底栏）。 */
const CONTENT = { x: 0, y: 40, width: 240, height: 160 };
test('方向键把焦点环画到第一处可点控件上', async ({ app }) => {
  await app.goto();
  // 没有手柄输入时不留焦点：第一处控件上没有环。
  expect(await app.brightShare(USB_CIRCLE)).toBeLessThan(0.004);

  await app.page.keyboard.down('ArrowRight');
  await app.waitFrames(2);
  // 键盘按键位进的是同一份 frame 契约，预览页读数就是送进去的掩码
  // （0x0020 = PocketJS 的右键位）。
  expect((await app.readout()).keys).toBe('0020');
  await app.page.keyboard.up('ArrowRight');
  await app.waitFrames(2);

  await expect.poll(() => app.brightShare(USB_CIRCLE)).toBeGreaterThan(0.012);
  // 遍历从当前页开始，底栏此刻还没轮到。
  expect(await app.brightShare(NAV_BAR)).toBeLessThan(0.004);
});

test('回车激活聚焦的控件：与点屏幕同一条通路', async ({ app }) => {
  await app.goto();
  // 首页左圆是「进模式页」：方向键聚焦后回车，等价于点它。
  await app.pad.press('ArrowRight');
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('串口')).toBe(true);
});

test('WASD 与空格跟方向键、回车等价', async ({ app }) => {
  await app.goto();
  await app.pad.press('KeyD');
  await app.pad.press('Space');
  await expect.poll(() => app.hasVisibleText('串口')).toBe(true);
});

test('连按方向键不会走进隐藏页面：回车只落在看得见的控件上', async ({ app }) => {
  await app.goto();
  const home = await app.regionSignature(CONTENT);

  // 首页只有两枚状态圆，再往后是底栏：第三次右移应当落在底栏「状态」上。
  // 若按整棵节点树遍历（首帧就把七个页面挂满、切页只翻 hidden），第三下会
  // 落到隐藏的设置页首行，回车就会切到操纵台看不见的那一页去。
  await app.pad.pressTimes('ArrowRight', 3);
  await app.pad.press('Enter');

  await expect.poll(() => app.hasVisibleText('Pro 手柄')).toBe(false);
  await expect.poll(() => app.regionSignature(CONTENT)).toBe(home);
  // 焦点落在底栏，环画在底栏里。
  expect(await app.brightShare(NAV_BAR)).toBeGreaterThan(0.008);
});

test('方向键能停在底栏最右侧的按钮上，回车切到设置页', async ({ app }) => {
  await app.goto();
  // 首页两枚状态圆之后是底栏两键：第四次右移应当落在「设置」上。
  await app.pad.pressTimes('ArrowRight', 4);
  expect(await app.brightShare(SETTINGS_KEY, RING_LEVEL)).toBeGreaterThan(0.004);
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('手柄配对')).toBe(true);
});

test('方向键移到列表下方时内容跟着滚动，焦点不被底栏挡住', async ({ app }) => {
  await app.goto();
  await openSettings(app);
  // 静止时确实没有环（这条同时钉住阈值本身不会误判页面文字）。
  expect(await ringBands(app, 34, 206)).toHaveLength(0);
  // 设置页五行：连按五次下移停在最后一行「调试」上，此时它已在底栏之下。
  await app.pad.pressTimes('ArrowDown', 5);
  // 环必须出现在底栏之上的内容区里；被底栏盖住时这里一条带都测不到。跟随
  // 滚动是 140 ms 的补间，等它落位再判。
  await expect.poll(async () => (await ringBands(app, 34, 206)).length).toBeGreaterThan(0);
});
