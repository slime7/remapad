/**
 * 连接与回连：设备对外只有一台 Pro Controller 2。
 *
 * 首次连接走配对流程（发现广播，主机侧注册凭证），断开后凭证还在，再按连接键
 * 走回连——不会再回到「扫描中…」。序号灯只在首页绘制，采样颜色前先回首页。
 */
import { test, expect, PLAYER_LED, type RemapadApp } from './fixtures';
import { goHome, openPairing } from './pages';

/** 开机静默：按连接键把当前身份交给主机（mock 里未配对走发现广播，约 8 秒）。 */
async function connectFirstTime(app: RemapadApp): Promise<void> {
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 20_000 }).toBe(true);
  await goHome(app);
}

test('首次连接走配对流程：配对完成后序号灯亮起', async ({ app }) => {
  await app.goto();
  await connectFirstTime(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 20_000 })
    .toBe(PLAYER_LED.on);
});

test('断开后凭证还在：再按连接键走回连，不进配对流程', async ({ app }) => {
  await app.goto();
  await connectFirstTime(app);

  // 断开：主机侧序号灯清零，凭证不受影响。
  await openPairing(app);
  await app.tapText('断开');
  await expect.poll(() => app.hasVisibleText('已配对')).toBe(true);
  await goHome(app);
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 10_000 })
    .toBe(PLAYER_LED.off);

  // 已配对身份按连接键发回连形态，主机直接连回来，不重新配对。
  await openPairing(app);
  await app.tapText('连接');
  await expect.poll(() => app.hasVisibleText('连接中…')).toBe(true);
  expect(await app.hasVisibleText('扫描中…'), '已配对身份不该进配对流程').toBe(false);
  await expect.poll(() => app.hasVisibleText('已连接'), { timeout: 15_000 }).toBe(true);
});

