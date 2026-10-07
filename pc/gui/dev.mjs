#!/usr/bin/env node
// GUI 开发入口：vite 以中间件并入后端 HTTP 服务，单进程单端口，改前端即热更新。
// 直跑 node、不经 pnpm wrapper，Windows 控制台 Ctrl+C 能干净退出不留孤儿进程。
import process from 'node:process';
import { fileURLToPath } from 'node:url';

import { createServer as createViteServer } from 'vite';

import { createGuiApp, waitForExit } from '../src/gui-server/app.js';
import { defaultDistDir } from '../src/gui-server/server.js';

export async function main(argv = process.argv.slice(2)) {
  const hostIndex = argv.indexOf('--host');
  const host = hostIndex >= 0 ? argv[hostIndex + 1] : '127.0.0.1';
  const portIndex = argv.indexOf('--http-port');
  const port = portIndex >= 0 ? Number(argv[portIndex + 1]) : 8787;

  const { server, gui, guiHandler, setHandler } = createGuiApp(defaultDistDir());
  const vite = await createViteServer({
    root: fileURLToPath(new URL('.', import.meta.url)),
    appType: 'spa',
    server: { middlewareMode: true, hmr: { server } },
  });
  setHandler((request, response) => {
    const path = new URL(request.url, 'http://localhost').pathname;
    if (path.startsWith('/api/')) {
      guiHandler(request, response);
      return;
    }
    vite.middlewares(request, response);
  });

  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
  console.log(`Remapad 连接控制台（vite 热更新）：http://${host}:${port}/`);
  gui.refreshPorts().catch(() => {});
  gui.refreshPads().catch(() => {});
  await waitForExit(server, gui);
  await vite.close();
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
