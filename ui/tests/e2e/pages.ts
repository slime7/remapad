/**
 * 页面级操作：在新四叶草左右滑动轮播架构下，提供滑动手势与页面切换辅助函数。
 */
import { expect, type RemapadApp } from './fixtures';

/** 上半区域向左滑（超过 80px 阈值，切到下一页）。 */
export async function swipeNext(app: RemapadApp): Promise<void> {
  await app.touch.flick({ x: 190, y: 100 }, { x: 40, y: 100 });
  await app.waitSettled();
  await app.refreshTree();
}

/** 上半区域向右滑（超过 80px 阈值，切到上一页）。 */
export async function swipePrev(app: RemapadApp): Promise<void> {
  await app.touch.flick({ x: 40, y: 100 }, { x: 190, y: 100 });
  await app.waitSettled();
  await app.refreshTree();
}

/** 回到第 1 页（亮度调节页）。 */
export async function goHome(app: RemapadApp): Promise<void> {
  for (let i = 0; i < 6; i++) {
    if (await app.hasVisibleText('2')) return;
    await swipeNext(app);
  }
}

/** 打开第 2 页：手柄设置页。 */
export async function openControllerSettings(app: RemapadApp): Promise<void> {
  await goHome(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('SN: HEJ71001123456')).toBe(true);
}

/** 打开第 3 页：手柄配对页。 */
export async function openPairing(app: RemapadApp): Promise<void> {
  await openControllerSettings(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('配对')).toBe(true);
}

/** 打开第 4 页：电源管理页。 */
export async function openPower(app: RemapadApp): Promise<void> {
  await openPairing(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('重启')).toBe(true);
}

/** 打开第 5 页：DS4、DS5 设置页。 */
export async function openDsSettings(app: RemapadApp): Promise<void> {
  await openPower(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('DS4、DS5 设置')).toBe(true);
}

/** 打开第 6 页：系统信息页。 */
export async function openSystemInfo(app: RemapadApp): Promise<void> {
  await openDsSettings(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('设备信息')).toBe(true);
}

/** 打开第 7 页：调试页。 */
export async function openDebug(app: RemapadApp): Promise<void> {
  await openSystemInfo(app);
  await swipeNext(app);
  await expect.poll(() => app.hasVisibleText('调试指令')).toBe(true);
}
