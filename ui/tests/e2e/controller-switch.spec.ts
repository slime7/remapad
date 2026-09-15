/**
 * 手柄类型切换：切换等价于「旧手柄断电、新手柄上电」。
 *
 * 断开后主机不再认这台手柄，玩家序号灯随之清零；新手柄（浏览器 mock 里
 * JoyCon 组合从未配对过）自动进入配对流程，配对完成后作为另一台设备重新
 * 亮灯。切回已配对的 Pro 则走回连：凭证还在，屏幕不会再回到「扫描中…」。
 *
 * 序号灯只在首页绘制，采样颜色前先回首页。
 */
import { test, expect, PLAYER_LED } from './fixtures';
import { goHome, openControllerSettings, openPairing } from './pages';

/** 两张类型卡右侧留白处的采样点（与 controller-settings 用例同坐标）。 */
const CARD_PRO = { x: 200, y: 60 };
const CARD_JOYCON = { x: 200, y: 121 };

test('切到未配对的 JoyCon 组合：序号灯清零，按新手柄流程重新连接', async ({ app }) => {
  await app.goto();
  // 先等 Pro 身份被主机注册：序号灯亮起。
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

  // 新手柄没有凭证：自动进入配对流程等主机搜索。
  await openPairing(app);
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

test('切回已配对的 Pro：直接回连，不再进配对流程', async ({ app }) => {
  await app.goto();
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 20_000 })
    .toBe(PLAYER_LED.on);

  // 换到 JoyCon 组合并等它配对完成（此刻 Pro 与 JoyCon 各有一份凭证）。
  await openControllerSettings(app);
  await app.touch.tap(CARD_JOYCON.x, CARD_JOYCON.y);
  await openPairing(app);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);

  // 切回 Pro：凭证还在，走回连——屏幕不会再回到「扫描中…」。
  await openControllerSettings(app);
  await app.touch.tap(CARD_PRO.x, CARD_PRO.y);
  await openPairing(app);
  expect(await app.hasVisibleText('扫描中…'), '已配对身份不该再进配对流程').toBe(false);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 15_000 }).toBe(true);
});
