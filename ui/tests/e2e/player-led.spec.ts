/**
 * 首页玩家序号指示灯：NS2 主机注册手柄后用 Command 0x09 返回 4 位掩码，
 * 首页两枚状态圆下方居中显示对应点亮的绿色方块（bit0-3 对应从左到右四格）。
 *
 * 浏览器 mock 里没有主机，配对流程走完即按 Player 1（掩码最低位）下发；
 * 没有下发时四格全灭。
 */
import { test, expect, PLAYER_LED } from './fixtures';
import { goHome, openPairing } from './pages';

test('主机返回玩家序号后首页四格指示灯亮起', async ({ app }) => {
  await app.goto();

  // 还没有主机下发序号：四格全灭。
  for (const x of PLAYER_LED.xs) {
    expect(await app.colorAt(x, PLAYER_LED.y), `第 ${x} 列不该点亮`).toBe(PLAYER_LED.off);
  }

  // 配对流程走完，主机下发 Player 1：只有第一格点亮。
  await openPairing(app);
  await app.tapText('开始');
  await expect.poll(() => app.hasVisibleText('已配对'), { timeout: 15_000 }).toBe(true);
  await goHome(app);

  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y))
    .toBe(PLAYER_LED.on);
  for (const x of PLAYER_LED.xs.slice(1)) {
    expect(await app.colorAt(x, PLAYER_LED.y), `第 ${x} 列不该点亮`).toBe(PLAYER_LED.off);
  }
});
