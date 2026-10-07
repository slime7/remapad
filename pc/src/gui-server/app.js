// GUI 装配工厂：HTTP + /ws 推流 + GuiServer 一套；cli.js（静态生产）与 pc/gui/dev.mjs（vite 热更新）共用。
import http from 'node:http';

import { WebSocketServer } from 'ws';

import { createRequestHandler, GuiServer } from './server.js';

/** 装配 GUI 应用；wrapHandler 传入时替换请求入口（dev 模式借此把非 /api 请求交给 vite）。 */
export function createGuiApp(distDir) {
  const server = http.createServer();
  const wss = new WebSocketServer({ noServer: true });
  const gui = new GuiServer({
    record: (record) => {
      const message = JSON.stringify({ t: 'records', items: [record] });
      for (const client of wss.clients) {
        if (client.readyState === client.OPEN) {
          client.send(message);
        }
      }
    },
  });
  const guiHandler = createRequestHandler(gui, distDir);
  let handler = guiHandler;
  server.on('request', (request, response) => handler(request, response));
  server.on('upgrade', (request, socket, head) => {
    if (new URL(request.url, 'http://localhost').pathname !== '/ws') {
      return; // 其余 upgrade（如 vite HMR）由各自注册的监听者处理。
    }
    wss.handleUpgrade(request, socket, head, (client) => {
      wss.emit('connection', client, request);
    });
  });
  wss.on('connection', (client) => {
    client.send(JSON.stringify({ t: 'records', items: gui.recent }));
  });
  return { server, gui, guiHandler, setHandler: (next) => { handler = next; } };
}

/** 挂住进程直到 Ctrl+C 或 SIGTERM：先停会话再断连接；exit 时兜底停会话。
 *  另设两道保险：收尾开始后 1.5 秒强退（防 vite 等收尾挂起）；轮询父进程，
 *  包裹链（pnpm dev）被杀而信号到不了子进程时自行收尾，不留孤儿占端口。 */
export async function waitForExit(server, gui) {
  await new Promise((resolve) => {
    let closed = false;
    const watchdog = setInterval(() => {
      if (!process.ppid) {
        return;
      }
      try {
        process.kill(process.ppid, 0);
      } catch (exc) {
        if (exc.code === 'ESRCH') {
          close();
        }
      }
    }, 2000);
    watchdog.unref();
    const close = () => {
      if (closed) {
        return;
      }
      closed = true;
      clearInterval(watchdog);
      gui.shutdown();
      server.closeAllConnections();
      server.close(resolve);
      setTimeout(() => process.exit(0), 1500).unref();
    };
    process.on('SIGINT', close);
    process.on('SIGTERM', close);
  });
  process.on('exit', () => gui.shutdown());
}
