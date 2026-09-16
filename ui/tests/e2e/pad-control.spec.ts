/**
 * 手柄操控屏幕：方向键移动焦点、圆圈键确认。
 *
 * 真机上这两件事由固件的手柄操控模式驱动（组合键把输入从主机收给屏幕，
 * 见 docs/adr/0028）；浏览器预览里键盘随时可用，按键位经预览页送进同一套
 * frame 契约，所以「按得对、画得出」与真机是同一条聚焦与激活通路。
 */
import { test, expect, NAV_BAR, type RemapadApp } from './fixtures';
import { openControllerSettings, openSettings, openSystem } from './pages';

/** 底栏右侧「设置」键（left/right/bottom 各 8，键宽 64）：取整块，环画在边界上。 */
const SETTINGS_KEY = { x: 168, y: 208, width: 64, height: 64 };
/** 底栏左侧「状态」键（撑满余下宽度的宽键）：取整块，环画在边界上。 */
const STATUS_KEY = { x: 8, y: 208, width: 152, height: 64 };

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
/** 首页右侧状态圆：蓝牙 / 配对（行内间距 8，两圆一起居中）。 */
const BT_CIRCLE = { x: 124, y: 38, width: 56, height: 56 };
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

test('连按向下不会走进隐藏页面：环停在首页第二枚圆上', async ({ app }) => {
  await app.goto();
  // 首页只有两枚状态圆：第三次下移没有下一项，环留在第二枚圆上，不会落到
  // 隐藏页的控件（首帧就把七个页面挂满、切页只翻 hidden）。回车进的是第二
  // 枚圆自己的配对页，而不是隐藏页首行的「手柄设置」。
  await app.pad.pressTimes('ArrowDown', 3);
  expect(await app.brightShare(BT_CIRCLE, RING_LEVEL)).toBeGreaterThan(0.004);
  expect(await app.brightShare(NAV_BAR)).toBeLessThan(0.004);
  await app.pad.press('Enter');
  await expect.poll(() => app.hasVisibleText('序列号 HEJ71001123456')).toBe(false);
  await expect.poll(() => app.hasVisibleText('配对')).toBe(true);
});

test('方向键能停在底栏最右侧的按钮上，回车切到设置页', async ({ app }) => {
  await app.goto();
  // 第一次按键由框架的默认顺序落点（首页首枚状态圆），第二次右移就进底栏：
  // 左右只在底栏两项之间走，不会先绕去第二枚圆。
  await app.pad.pressTimes('ArrowRight', 2);
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

test('左右只在底栏两项之间走，不落进页面内容', async ({ app }) => {
  await app.goto();
  await openSettings(app);
  // 第一次按键进内容第一行，右移一次就进底栏右端的「设置」键。
  await app.pad.press('ArrowDown');
  await app.pad.press('ArrowRight');
  expect(await app.brightShare(SETTINGS_KEY, RING_LEVEL)).toBeGreaterThan(0.004);
  expect(await ringBands(app, 34, 206)).toHaveLength(0);
  // 左移：环走到「状态」；到头再按左仍夹在底栏里，不会退进页面内容。
  await app.pad.press('ArrowLeft');
  expect(await app.brightShare(STATUS_KEY, RING_LEVEL)).toBeGreaterThan(0.004);
  await app.pad.press('ArrowLeft');
  expect(await app.brightShare(STATUS_KEY, RING_LEVEL)).toBeGreaterThan(0.004);
  expect(await ringBands(app, 34, 206)).toHaveLength(0);
});

test('上下只在页面内容里走，连按到底也不会停到底栏上', async ({ app }) => {
  await app.goto();
  await openSettings(app);
  // 设置页五行：连按六次下移，多按的那一次已经没有下一项。
  await app.pad.pressTimes('ArrowDown', 6);
  expect(await app.brightShare(NAV_BAR)).toBeLessThan(0.004);
  await expect.poll(async () => (await ringBands(app, 34, 206)).length).toBeGreaterThan(0);
});

test('环停在底栏时按上回到页面内容', async ({ app }) => {
  await app.goto();
  await openSettings(app);
  await app.pad.press('ArrowDown');
  await app.pad.press('ArrowRight');
  expect(await app.brightShare(SETTINGS_KEY, RING_LEVEL)).toBeGreaterThan(0.004);
  await app.pad.press('ArrowUp');
  expect(await app.brightShare(NAV_BAR)).toBeLessThan(0.004);
  await expect.poll(async () => (await ringBands(app, 34, 206)).length).toBeGreaterThan(0);
});

test('焦点停在最后一项后继续按下，页面还能一直滚到页底', async ({ app }) => {
  await app.goto();
  await openSystem(app);
  const content = await app.findVisibleByClass('px-4 pt-[34]');
  expect(content, '没找到系统页滚动列').toBeDefined();
  // 系统页四个可聚焦行：连按四下停在最后一行「关机」上，跟随滚动已经把它送进
  // 可视带，但下面的设备信息卡还看不见。
  await app.pad.pressTimes('ArrowDown', 4);
  await app.waitSettled();
  const before = await app.scrollOffset(content!.i);
  expect(before, '系统页应当已经跟着焦点滚过一段').not.toBeNull();
  // 再按下：焦点没有下一项可去，页面自己继续往下走，一直走到页底。内容高
  // 34 + 56 + 16×3 + 44×2 + 170 + 80 = 476，视口 280，页底就是 196。
  await app.pad.pressTimes('ArrowDown', 8);
  await app.waitSettled();
  const after = await app.scrollOffset(content!.i);
  expect(after, '系统页应当能滚到页底').not.toBeNull();
  expect(after!).toBeGreaterThan(before!);
  expect(after!).toBe(196);
});

test('手柄设置页内容单屏放得下：按住下也不会滚动', async ({ app }) => {
  await app.goto();
  await openControllerSettings(app);
  const content = await app.findVisibleByClass('px-4 pt-[34]');
  expect(content, '没找到手柄设置页滚动列').toBeDefined();
  // 页面上只有信息卡与一行四个配色按钮：内容高 34 + 54（信息卡）+
  // 48（配色行）+ 8×2 = 152，加末尾垫高 80 得 232，视口 280——单屏放得下，
  // 焦点在四个色块之间移动时位移恒为 0。
  await app.pad.pressTimes('ArrowDown', 8);
  await app.waitSettled();
  await expect
    .poll(async () => app.scrollOffset(content!.i), { message: '内容单屏放得下，位移应当保持 0' })
    .toBe(0);
});
