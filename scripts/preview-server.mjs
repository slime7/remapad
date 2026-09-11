// 触摸屏预览服务器。只提供三类内容：本仓库的预览页面、UI 构建产物（ui/dist），
// 以及 @pocketjs/framework 自带的浏览器运行时（官方 wasm 核心与宿主绑定）。
// 官方 hosts/web 的 playground 页面面向 PSP 按键，本项目使用触摸预览页，因此由本脚本
// 负责静态服务，渲染与触摸语义仍然来自官方运行时。
import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { extname, join, resolve, sep } from 'node:path';

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.wasm': 'application/wasm',
  '.pak': 'application/octet-stream',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
};

/** 把 URL 路径解析到根目录内，越出根目录的路径一律拒绝。 */
function resolveInside(root, relative) {
  const normalizedRoot = resolve(root);
  const target = resolve(normalizedRoot, '.' + sep + relative.replace(/^[/\\]+/, ''));
  if (target !== normalizedRoot && !target.startsWith(normalizedRoot + sep)) {
    return null;
  }
  return target;
}

async function sendFile(response, filePath) {
  const info = await stat(filePath);
  if (!info.isFile()) {
    throw new Error('not a file');
  }
  const body = await readFile(filePath);
  response.writeHead(200, {
    'content-type': MIME[extname(filePath).toLowerCase()] ?? 'application/octet-stream',
    'content-length': body.length,
    'cache-control': 'no-store',
  });
  response.end(body);
}

export function startPreviewServer({ port = 8130, pageDir, distDir, runtimeDir }) {
  const routes = [
    { prefix: '/preview/', root: resolve(pageDir) },
    { prefix: '/dist/', root: resolve(distDir) },
    { prefix: '/runtime/', root: resolve(runtimeDir) },
  ];
  const indexFile = join(resolve(pageDir), 'index.html');
  /** 已连接的事件流响应，用于在重新编译后通知页面加载新产物。 */
  const eventClients = new Set();

  const server = createServer((request, response) => {
    const path = decodeURIComponent(new URL(request.url ?? '/', 'http://127.0.0.1').pathname);
    if (path === '/events') {
      response.writeHead(200, {
        'content-type': 'text/event-stream; charset=utf-8',
        'cache-control': 'no-cache',
        connection: 'keep-alive',
      });
      response.write('data: connected\n\n');
      eventClients.add(response);
      request.on('close', () => eventClients.delete(response));
      return;
    }
    const route = routes.find((entry) => path.startsWith(entry.prefix));
    let filePath;
    if (path === '/' || path === '/index.html') {
      filePath = indexFile;
    } else if (route) {
      filePath = resolveInside(route.root, path.slice(route.prefix.length));
    } else {
      filePath = null;
    }
    if (filePath === null) {
      response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
      response.end('未找到: ' + path);
      return;
    }
    sendFile(response, filePath).catch(() => {
      response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
      response.end('未找到: ' + path);
    });
  });

  return new Promise((resolveServer, rejectServer) => {
    server.once('error', rejectServer);
    server.listen(port, '127.0.0.1', () => {
      resolveServer({
        port,
        url: `http://127.0.0.1:${port}/`,
        /** 通知已连接的预览页重新加载 ui/dist 中的产物。 */
        notifyReload() {
          for (const client of eventClients) {
            client.write('data: reload\n\n');
          }
          return eventClients.size;
        },
        close: () => server.close(),
      });
    });
  });
}
