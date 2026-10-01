// 端到端配置：webServer 自动拉起预览页静态服务，用例跑在无头 Chromium 里；
// 默认视口是窄屏（预览在上、控制台在下），宽屏布局由用例自开上下文验证。
import { defineConfig } from "@playwright/test";

export default defineConfig({
  testDir: "./e2e",
  timeout: 15000,
  use: {
    baseURL: "http://127.0.0.1:8123/",
    viewport: { width: 360, height: 700 },
    deviceScaleFactor: 1,
  },
  webServer: {
    command: "node tools/serve.mjs --port 8123",
    url: "http://127.0.0.1:8123/",
    reuseExistingServer: true,
    timeout: 10000,
  },
});
