/**
 * 手柄类型切换：切换等价于「旧手柄断电、新手柄上电」。
 *
 * 断开后主机不再认这台手柄，玩家序号灯随之清零；新手柄（浏览器 mock 里
 * JoyCon 组合从未配对过）不自动发信号，按连接键才进配对流程，配对完成后
 * 作为另一台设备重新亮灯。切回已配对的 Pro 则按连接键走回连：凭证还在，
 * 屏幕不会再回到「扫描中…」。
 *
 * 序号灯只在首页绘制，采样颜色前先回首页。
 */
import { test, expect, PLAYER_LED, type RemapadApp } from './fixtures';
import { goHome, openControllerSettings, openPairing } from './pages';

/** 两张类型卡右侧留白处的采样点（与 controller-settings 用例同坐标）。 */
const CARD_PRO = { x: 200, y: 60 };
const CARD_JOYCON = { x: 200, y: 121 };

/** 开机静默：按连接键把当前身份交给主机（mock 里未配对走发现广播，约 8 秒）。 */
async function connectFirstTime(app: RemapadApp): Promise<void> {
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
  await goHome(app);
}

test('切到未配对的 JoyCon 组合：序号灯清零，按连接键进配对流程', async ({ app }) => {
  await app.goto();
  // 先按连接键把 Pro 身份交给主机：序号灯亮起。
  await connectFirstTime(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 20_000 })
    .toBe(PLAYER_LED.on);

  await openControllerSettings(app);
  await app.touch.tap(CARD_JOYCON.x, CARD_JOYCON.y);
  // 旧手柄断电：主机侧序号灯清零（新身份是另一台设备，需要重新配对）。
  await goHome(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 10_000 })
    .toBe(PLAYER_LED.off);

  // 新手柄没有凭证也不自动发信号：静默等用户按连接键。
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('未配对')).toBe(true);
  expect(await app.hasVisibleText('扫描中…'), '不按键就不该广播').toBe(false);

  // 连接键（未配对身份）进配对流程等主机搜索。
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('扫描中…')).toBe(true);

  // 配对完成：新手柄被注册，序号灯重新亮起。
  await expect
    .poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 })
    .toBe(true);
  await goHome(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 10_000 })
    .toBe(PLAYER_LED.on);
});

test('切回已配对的 Pro：按连接键回连，不再进配对流程', async ({ app }) => {
  await app.goto();
  await connectFirstTime(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 20_000 })
    .toBe(PLAYER_LED.on);

  // 换到 JoyCon 组合并等它配对完成（此刻 Pro 与 JoyCon 各有一份凭证）。
  await openControllerSettings(app);
  await app.touch.tap(CARD_JOYCON.x, CARD_JOYCON.y);
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);

  // 切回 Pro：凭证还在，静默等连接键；连接后走回连，不会再回到「扫描中…」。
  await openControllerSettings(app);
  await app.touch.tap(CARD_PRO.x, CARD_PRO.y);
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('已配对')).toBe(true);
  expect(await app.hasVisibleText('扫描中…'), '已配对身份不该再进配对流程').toBe(false);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('连接中…')).toBe(true);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 15_000 }).toBe(true);
});
