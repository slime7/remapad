/**
 * 顶部状态栏：图标与颜色必须跟随当前状态，不能停在首帧算出来的外观。
 *
 * 状态栏的外观（图标字形与颜色）是在组件函数体里算好的，而 Vue Vapor 只在
 * 首帧执行一次函数体：派生值包成函数、在 JSX 内调用才会被渲染作用跟踪
 * （同 AppNavBar / HomePage / PairingPage 的注释）。违反这条时文本仍会随
 * 响应式更新、图标与颜色却冻在首帧——设备上首帧早于固件应答，电量图标因此
 * 停在 0% 的红色告警，看上去像「电量不足」，而数字已经是真实电量。
 *
 * 浏览器里 mock 是同步应答，首帧拿到的就是真值，复现不了设备上的时序；能
 * 复现同一条缺陷的是模式切换：文本换成 HOST，图标必须跟着变成未连接。
 */
import { test, expect, type RemapadApp } from './fixtures';
import { openMode } from './pages';

/** icons.tsx 里的字形：图标是字体的 PUA 文本节点。 */
const GLYPH_USB = '\ue1e0';
const GLYPH_USB_OFF = '\ue4fa';
const GLYPH_BATTERY_STD = '\ue1a5';
const GLYPH_BATTERY_ALERT = '\ue19c';

/** 状态栏子树里的文本（含图标字形），与页面上的同名图标区分开。 */
async function statusBarTexts(app: RemapadApp): Promise<string[]> {
  const nodes = await app.nodes();
  const rootIndex = nodes.findIndex((node) => node.c?.includes('bg-[#081423b3]') === true);
  expect(rootIndex, '没找到状态栏节点').toBeGreaterThanOrEqual(0);
  const under = (index: number): boolean => {
    for (let at = index; at >= 0; at = nodes[at].p) {
      if (at === rootIndex) {
        return true;
      }
    }
    return false;
  };
  return nodes
    .filter((node, index) => under(index) && node.x !== undefined && node.x !== '')
    .map((node) => node.x!);
}

test('状态栏图标跟随状态：切到手柄模式后 USB 图标变为未连接', async ({ app }) => {
  await app.goto();

  // 88% 不是低电量：图标是通用电池字形，不能是红色告警字形。
  const before = await statusBarTexts(app);
  expect(before).toContain(GLYPH_USB);
  expect(before).toContain(GLYPH_BATTERY_STD);
  expect(before).not.toContain(GLYPH_BATTERY_ALERT);

  await openMode(app);
  await app.tapText('手柄');

  // 角色短标先换成 HOST，图标紧随其后换成未连接字形。
  await expect.poll(() => app.hasVisibleText('HOST')).toBe(true);
  await expect.poll(() => statusBarTexts(app)).toContain(GLYPH_USB_OFF);
  expect(await statusBarTexts(app)).not.toContain(GLYPH_USB);
});
