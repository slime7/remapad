/**
 * 首页玩家序号指示灯：NS2 主机注册手柄后用 Command 0x09 返回 4 位掩码，
 * 首页两枚状态圆下方居中显示对应点亮的绿色方块（bit0-3 对应从左到右四格）。
 *
 * 浏览器 mock 里主机侧配对走完（约 8 秒）即按 Player 1（掩码最低位）下发；
 * 没有下发时四格全灭。设备侧不需要任何手动操作——开机自动进入配对流程。
 */
import { test, expect, PLAYER_LED } from './fixtures';

test('主机返回玩家序号后首页四格指示灯亮起', async ({ app }) => {
  await app.goto();

  // 还没有主机注册：四格全灭。
  for (const x of PLAYER_LED.xs) {
    expect(await app.colorAt(x, PLAYER_LED.y), `第 ${x} 列不该点亮`).toBe(PLAYER_LED.off);
  }

  // 主机侧配对完成（固件全程自动），下发 Player 1：只有第一格点亮。
  await expect
    .poll(() => app.colorAt(PLAYER_LED.xs[0], PLAYER_LED.y), { timeout: 20_000 })
    .toBe(PLAYER_LED.on);
  for (const x of PLAYER_LED.xs.slice(1)) {
    expect(await app.colorAt(x, PLAYER_LED.y), `第 ${x} 列不该点亮`).toBe(PLAYER_LED.off);
  }
});
