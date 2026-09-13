/**
 * UI 端到端测试配置：用真机同款的渲染核心（官方 wasm）驱动 ui/src，
 * 断言页面行为与屏幕像素。测试对象是 ui/dist 里的真实产物，因此
 * 运行前会先执行一次官方编译（由 webServer 命令完成）。
 *
 * 触摸屏是唯一输入方式，测试台把指针事件换成设备触点，详见
 * tests/e2e/fixtures.ts 与 docs/TESTING.md。
 */
import { defineConfig, devices } from '@playwright/test';

const PORT = 8130;
const BASE_URL = `http://127.0.0.1:${PORT}`;

export default defineConfig({
  testDir: './tests/e2e',
  // 纯 CPU 的软件光栅化核心 + 单进程预览服务器：并发只会互相拖慢。
  fullyParallel: false,
  workers: 1,
  forbidOnly: Boolean(process.env.CI),
  retries: 0,
  timeout: 90_000,
  expect: { timeout: 15_000 },
  reporter: process.env.CI ? [['list'], ['html', { open: 'never' }]] : [['list']],
  use: {
    baseURL: BASE_URL,
    // 屏幕按 2 倍显示，画布占 480 × 560，两侧还要放下读数面板。
    viewport: { width: 1280, height: 820 },
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: { ...devices['Desktop Chrome'], viewport: { width: 1280, height: 820 } },
    },
  ],
  webServer: {
    // 与 pnpm run dev 同一条命令：编译产物后拉起触摸预览（8130）与官方
    // DevTools 服务器（8131）。本地已有 dev 会话时直接复用。
    command: 'node ../scripts/pocketjs.mjs web',
    url: BASE_URL,
    reuseExistingServer: !process.env.CI,
    timeout: 300_000,
    stdout: 'pipe',
    stderr: 'pipe',
  },
});
