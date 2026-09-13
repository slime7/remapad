/**
 * 启动与首屏：应用首帧就绪后的可见事实。
 *
 * 这一组用例是「UI 挂掉」的兜底：编译产物缺失、首页没建起来、渲染器捕获到
 * 异常，都会在这里失败；同时守住 ADR 0016 的约定——七个页面在首屏前一次挂完。
 */
import { test, expect } from './fixtures';

test('首帧就绪，首页与状态栏都画出来了', async ({ app }) => {
  await app.goto();
  const readout = await app.readout();
  expect(readout.status).toContain('运行中');
  expect(readout.log).toContain('已加载 remapad-ui');
  expect(app.consoleLines.filter((line) => line.includes('[pageerror]'))).toEqual([]);
  // 应用把渲染器捕获到的异常交给 console.error 上报，这里不能有 error。
  expect((await app.appConsole()).filter((line) => line.level === 'error')).toEqual([]);

  // 控制面握手后状态栏显示 USB 角色短标与电量。
  const texts = await app.visibleTexts();
  expect(texts).toContain('COM');
  expect(texts).toContain('88%');
  // 首页两枚 56 见方的状态圆：取圆内、图标上方的点，应当是圆底色。
  expect(await app.colorAt(88, 46)).toBe('#14263e');
  expect(await app.colorAt(152, 46)).toBe('#14263e');
});

test('七个页面在首屏前一次挂完，未激活页面处于 hidden', async ({ app }) => {
  await app.goto();
  const nodes = await app.nodes();
  for (const label of ['手柄设置', '手柄配对', '模式切换', '调试']) {
    const matches = nodes.filter((node) => node.x === label);
    expect(matches.length, `「${label}」节点应当已建好`).toBe(1);
    expect(matches[0].hidden, `「${label}」应当处于 hidden`).toBe(true);
  }
  // 建好不等于显示：首页之外的页面此时都收起了。
  expect(await app.hasVisibleText('手柄设置')).toBe(false);
});
