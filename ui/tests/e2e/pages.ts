/**
 * 页面级操作：在四叶草左右滑动轮播架构下按页面内容导航。
 *
 * 页面清单只在本文件出现一次（顺序即轮播顺序）：新增或删除页面只改 PAGES，
 * 各页用例按「本页独有文案」打开，不写页面序号，也不依赖相邻页是谁。
 */
import { expect, type RemapadApp } from './fixtures';
import { IS_DEV } from '../../src/env.generated';

/**
 * 各页独有文案，顺序即左右翻页顺序：亮度档位数字 / 手柄设置的身份卡 /
 * 配对页的配对钮 / 电源页的重启钮 / USB 模式页标题 / DS 设置页标题 /
 * 系统信息页标题，dev 构建末尾接调试页。
 */
export const PAGES = [
  '2',
  'SN: HEJ71001123456',
  '配对',
  '重启',
  'USB 模式',
  'DS4、DS5 设置',
  '设备信息',
  ...(IS_DEV ? ['调试指令'] : []),
];

/** 轮播末页的独有文案（首页向右滑到的就是它）。 */
export const LAST_PAGE = PAGES[PAGES.length - 1];

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

/** 打开带该文案的页面：从当前页一路向左滑，最多绕一圈。 */
export async function openPage(app: RemapadApp, marker: string): Promise<void> {
  for (let step = 0; step < PAGES.length; step += 1) {
    if (await app.hasVisibleText(marker)) {
      return;
    }
    await swipeNext(app);
  }
  await expect.poll(() => app.hasVisibleText(marker)).toBe(true);
}

/** 回到首页：亮度调节页。 */
export async function goHome(app: RemapadApp): Promise<void> {
  await openPage(app, '2');
}

/** 手柄设置页。 */
export async function openControllerSettings(app: RemapadApp): Promise<void> {
  await openPage(app, 'SN: HEJ71001123456');
}

/** 手柄配对页。 */
export async function openPairing(app: RemapadApp): Promise<void> {
  await openPage(app, '配对');
}

/** 电源管理页。 */
export async function openPower(app: RemapadApp): Promise<void> {
  await openPage(app, '重启');
}

/** USB 模式页。 */
export async function openUsbMode(app: RemapadApp): Promise<void> {
  await openPage(app, 'USB 模式');
}

/** DS4、DS5 设置页。 */
export async function openDsSettings(app: RemapadApp): Promise<void> {
  await openPage(app, 'DS4、DS5 设置');
}

/** 系统信息页。 */
export async function openSystemInfo(app: RemapadApp): Promise<void> {
  await openPage(app, '设备信息');
}

/** 调试页（仅 dev 构建）。 */
export async function openDebug(app: RemapadApp): Promise<void> {
  await openPage(app, '调试指令');
}
