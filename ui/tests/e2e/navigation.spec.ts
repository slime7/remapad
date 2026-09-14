/**
 * 页面切换：底栏两键互切、导航激活态、设置页列表进入功能页。
 *
 * 页面可见性由各页根节点的 hidden 翻转（见 ADR 0016），所以断言分两层：
 * 组件树里的可见文本，以及像素级的「这一页画出来了吗、切回去还原了吗」。
 */
import { test, expect, NAV_SETTINGS, NAV_STATUS } from './fixtures';

/** 页面内容区：状态栏（26）与底栏（208 起）之间的部分。 */
const CONTENT = { x: 0, y: 40, width: 240, height: 160 };
const NAV_BAR_RECT = { x: 8, y: 208, width: 224, height: 64 };

test('底栏两键互切，导航激活态随之变化且切回后完全还原', async ({ app }) => {
  await app.goto();
  const homeContent = await app.regionSignature(CONTENT);
  const homeNav = await app.regionSignature(NAV_BAR_RECT);

  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(true);
  expect(await app.regionSignature(CONTENT)).not.toBe(homeContent);
  // 激活键换人：两键的 on/off 贴图不同，整条底栏的像素必须变。
  expect(await app.regionSignature(NAV_BAR_RECT)).not.toBe(homeNav);

  await app.touch.tap(NAV_STATUS.x, NAV_STATUS.y);
  await expect.poll(() => app.hasVisibleText('手柄设置')).toBe(false);
  // 切回首页后画面与底栏都还原，没有残留的选中态或半张页面。
  await expect.poll(() => app.regionSignature(CONTENT)).toBe(homeContent);
  await expect.poll(() => app.regionSignature(NAV_BAR_RECT)).toBe(homeNav);
});

test('设置页列表进入功能页，且同时只显示一页', async ({ app }) => {
  await app.goto();
  await app.touch.tap(NAV_SETTINGS.x, NAV_SETTINGS.y);
  await expect.poll(() => app.hasVisibleText('手柄配对')).toBe(true);

  // 列表首行「手柄设置」：pt-[34] 起、行高 44，中心在 y=56。
  await app.touch.tap(120, 56);
  // 「手柄设置」页的识别文本：顶部「手柄类型」标签已去掉，改用类型卡标题。
  await expect.poll(() => app.hasVisibleText('Pro 手柄')).toBe(true);
  expect(await app.hasVisibleText('手柄配对')).toBe(false);
  expect(await app.hasVisibleText('手柄设置')).toBe(false);
});
