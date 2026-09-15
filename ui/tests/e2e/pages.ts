/**
 * 页面级操作：把「从默认状态走到某个功能页」的动作收在一处，用例只关心
 * 被验证的行为，不再重复坐标与滚动细节。坐标取自页面布局常量
 * （设置页 pt-[34]、行高 44、行距 8；底栏两键中心固定）。
 */
import { expect, NAV_SETTINGS, NAV_STATUS, type RemapadApp } from './fixtures';

/** 设置页列表项顺序，与 ui/src/pages/SettingsPage.tsx 的 ITEMS 一致。 */
const SETTINGS_ITEMS = ['手柄设置', '手柄配对', '模式切换', '系统', '调试'] as const;
export type SettingsItem = (typeof SETTINGS_ITEMS)[number];

const ITEM_H = 44;
const ITEM_GAP = 8;
const TOP_PAD = 34;
/** 列表项中心 y（未滚动时）。 */
const itemCenter = (index: number) => TOP_PAD + index * (ITEM_H + ITEM_GAP) + ITEM_H / 2;

/** 回到状态页（用例之间互不影响）。 */
export async function goHome(app: RemapadApp): Promise<void> {
  await app.touch.tap(NAV_STATUS.x, NAV_STATUS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(false);
}

/** 打开设置页。 */
export async function openSettings(app: RemapadApp): Promise<void> {
  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄配对')).toBe(true);
  await app.refreshTree();
}

/**
 * 打开设置页里的某个列表项。末两项被底栏压住，先滚到底再按实测位移换算
 * 落点，避免把「滚动多少」写死在用例里。
 */
export async function openSettingsItem(app: RemapadApp, item: SettingsItem): Promise<void> {
  const index = SETTINGS_ITEMS.indexOf(item);
  await openSettings(app);
  let y = itemCenter(index);
  if (y > 200) {
    const content = await app.findVisibleByClass('px-4 pt-[34]');
    expect(content, '没找到设置页滚动列').toBeDefined();
    await app.touch.flick({ x: 120, y: 190 }, { x: 120, y: 140 });
    await app.waitSettled();
    const offset = await app.scrollOffset(content!.i);
    expect(offset, '设置页应当能滚到底').not.toBeNull();
    y -= offset!;
  }
  await app.touch.tap(120, y);
}

/** 打开手柄设置页（设置列表首行）。 */
export async function openControllerSettings(app: RemapadApp): Promise<void> {
  await openSettingsItem(app, '手柄设置');
  await expect.poll(() => app.hasVisibleText('Pro 手柄')).toBe(true);
  await app.refreshTree();
}

/** 打开模式页（设置列表第三行）。 */
export async function openMode(app: RemapadApp): Promise<void> {
  await openSettingsItem(app, '模式切换');
  await expect.poll(() => app.hasVisibleText('串口')).toBe(true);
  await app.refreshTree();
}

/**
 * 打开配对页（设置列表第二行）。配对流程可能从开机就在跑，页面一开始就是
 * 「扫描中…」，所以这里只等页面自身的「配对」副标题可见，状态由用例各自轮询。
 */
export async function openPairing(app: RemapadApp): Promise<void> {
  await openSettingsItem(app, '手柄配对');
  await expect.poll(() => app.hasVisibleText('配对')).toBe(true);
  await app.refreshTree();
}

/** 打开系统页（设置列表第四行，需要滚动）。 */
export async function openSystem(app: RemapadApp): Promise<void> {
  await openSettingsItem(app, '系统');
  await expect.poll(() => app.hasVisibleText('重启设备')).toBe(true);
  await app.refreshTree();
}
