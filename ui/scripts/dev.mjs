import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const ROOT = path.resolve(__dirname, '..');
const SRC = path.join(ROOT, 'src');
const DIST = path.join(ROOT, 'dist');
const HOST_WEB = path.join(ROOT, 'node_modules/@pocketjs/framework/hosts/web');
const HTML_FILE = path.join(ROOT, 'index.html');

const PORT = 8130;

let sseClients = [];

function notifyClients() {
  for (const res of sseClients) {
    try {
      res.write('data: reload\n\n');
    } catch {
      // ignore dropped connections
    }
  }
}

let isBuilding = false;
let pendingBuild = false;

function triggerBuild() {
  if (isBuilding) {
    pendingBuild = true;
    return;
  }
  isBuilding = true;
  console.log('[PocketJS Dev] Rebuilding app...');
  const child = spawn(process.execPath, [
    path.join(__dirname, 'pocket.mjs'),
    'compile',
  ], {
    cwd: ROOT,
    stdio: 'inherit',
  });
  child.on('close', (code) => {
    isBuilding = false;
    if (code === 0) {
      console.log('[PocketJS Dev] Rebuild succeeded, reloading browser preview...');
      notifyClients();
    }
    if (pendingBuild) {
      pendingBuild = false;
      triggerBuild();
    }
  });
}

// Watch src folder for changes
let debounceTimer = null;
fs.watch(SRC, { recursive: true }, (eventType, filename) => {
  if (debounceTimer) {
    clearTimeout(debounceTimer);
  }
  debounceTimer = setTimeout(() => {
    console.log('[PocketJS Dev] Detected change in ' + filename);
    triggerBuild();
  }, 200);
});

// Watch index.html for changes
if (fs.existsSync(HTML_FILE)) {
  let htmlDebounce = null;
  fs.watch(HTML_FILE, () => {
    if (htmlDebounce) {
      clearTimeout(htmlDebounce);
    }
    htmlDebounce = setTimeout(() => {
      console.log('[PocketJS Dev] Detected change in index.html, reloading browser preview...');
      notifyClients();
    }, 100);
  });
}

triggerBuild();
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1:' + PORT);

  if (url.pathname === '/api/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
    });
    res.write('data: connected\n\n');
    sseClients.push(res);
    req.on('close', () => {
      sseClients = sseClients.filter((c) => c !== res);
    });
    return;
  }

  if (url.pathname === '/' || url.pathname === '/index.html') {
    if (fs.existsSync(HTML_FILE)) {
      res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      fs.createReadStream(HTML_FILE).pipe(res);
      return;
    }
    res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
    res.end('Not Found: ' + HTML_FILE);
    return;
  }

  if (url.pathname === '/pocketjs.wasm') {
    const wasmPath = path.join(HOST_WEB, 'pocketjs.wasm');
    if (fs.existsSync(wasmPath)) {
      res.writeHead(200, { 'Content-Type': 'application/wasm' });
      fs.createReadStream(wasmPath).pipe(res);
      return;
    }
  }

  if (url.pathname === '/wasm-ops.js') {
    const jsPath = path.join(HOST_WEB, 'wasm-ops.js');
    if (fs.existsSync(jsPath)) {
      res.writeHead(200, { 'Content-Type': 'application/javascript; charset=utf-8' });
      fs.createReadStream(jsPath).pipe(res);
      return;
    }
  }

  if (url.pathname === '/dist/remapad-ui.pak') {
    const pakPath = path.join(DIST, 'remapad-ui.pak');
    if (fs.existsSync(pakPath)) {
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Cache-Control': 'no-cache',
      });
      fs.createReadStream(pakPath).pipe(res);
      return;
    }
  }

  if (url.pathname === '/dist/styles.bin') {
    const stylesPath = path.join(DIST, 'styles.bin');
    if (fs.existsSync(stylesPath)) {
      res.writeHead(200, {
        'Content-Type': 'application/octet-stream',
        'Cache-Control': 'no-cache',
      });
      fs.createReadStream(stylesPath).pipe(res);
      return;
    }
  }

  if (url.pathname === '/dist/remapad-ui.js') {
    const appPath = path.join(DIST, 'remapad-ui.js');
    if (fs.existsSync(appPath)) {
      res.writeHead(200, {
        'Content-Type': 'application/javascript; charset=utf-8',
        'Cache-Control': 'no-cache',
      });
      fs.createReadStream(appPath).pipe(res);
      return;
    }
  }

  res.writeHead(404, { 'Content-Type': 'text/plain' });
  res.end('Not Found: ' + url.pathname);
});

server.listen(PORT, '127.0.0.1', () => {
  console.log('=================================================');
  console.log(' PocketJS 浏览器模拟器服务已启动!');
  console.log(' 本地预览地址: http://127.0.0.1:' + PORT);
  console.log(' 正在监听 ui/src 文件变动并支持热重载...');
  console.log('=================================================');
});
