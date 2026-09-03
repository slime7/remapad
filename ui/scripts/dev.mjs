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

const PORT = 8130;
const VIEWPORT_WIDTH = 240;
const VIEWPORT_HEIGHT = 280;

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
  if (debounceTimer) clearTimeout(debounceTimer);
  debounceTimer = setTimeout(() => {
    console.log('[PocketJS Dev] Detected change in ' + filename);
    triggerBuild();
  }, 200);
});
triggerBuild();
const SIMULATOR_HTML = `<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <title>PocketJS ESP32-S3 模拟器预览</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    :root {
      color-scheme: dark;
      --bg: #0b0f19;
      --card-bg: #161e2e;
      --accent: #38bdf8;
      --border: #233148;
    }
    body {
      margin: 0;
      padding: 24px 16px;
      background: var(--bg);
      color: #e2e8f0;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 16px;
      min-height: 100vh;
      box-sizing: border-box;
    }
    header {
      text-align: center;
    }
    h1 {
      margin: 0 0 4px 0;
      font-size: 20px;
      font-weight: 700;
      color: #f8fafc;
    }
    .badge {
      display: inline-block;
      padding: 2px 8px;
      background: rgba(56, 189, 248, 0.15);
      color: var(--accent);
      border: 1px solid rgba(56, 189, 248, 0.3);
      border-radius: 9999px;
      font-size: 11px;
      font-weight: 600;
    }
    .device-shell {
      background: #1e293b;
      border: 2px solid #334155;
      border-radius: 28px;
      padding: 16px 14px 20px 14px;
      box-shadow: 0 20px 40px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.1);
      display: flex;
      flex-direction: column;
      align-items: center;
      gap: 12px;
    }
    .device-screen-bezel {
      background: #020617;
      border: 2px solid #0f172a;
      border-radius: 12px;
      padding: 4px;
      display: flex;
      justify-content: center;
      align-items: center;
      box-shadow: inset 0 2px 6px rgba(0,0,0,0.8);
    }
    #screen {
      width: ${VIEWPORT_WIDTH}px;
      height: ${VIEWPORT_HEIGHT}px;
      image-rendering: pixelated;
      background: #000;
      border-radius: 8px;
      cursor: pointer;
    }
    @media (min-width: 600px) {
      #screen {
        width: ${VIEWPORT_WIDTH * 1.5}px;
        height: ${VIEWPORT_HEIGHT * 1.5}px;
      }
    }
    .device-controls {
      display: flex;
      align-items: center;
      justify-content: space-between;
      width: 100%;
      padding: 0 6px;
      box-sizing: border-box;
    }
    .btn {
      background: #334155;
      color: #f1f5f9;
      border: 1px solid #475569;
      border-radius: 9999px;
      padding: 8px 16px;
      font-size: 12px;
      font-weight: 600;
      cursor: pointer;
      user-select: none;
      transition: all 0.1s;
    }
    .btn:hover {
      background: #475569;
    }
    .btn:active {
      background: var(--accent);
      color: #0f172a;
      transform: scale(0.96);
    }
    .info-panel {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 12px;
      padding: 12px 16px;
      max-width: 420px;
      width: 100%;
      font-size: 12px;
      line-height: 1.6;
      color: #94a3b8;
      box-sizing: border-box;
    }
    .info-row {
      display: flex;
      justify-content: space-between;
      padding: 3px 0;
    }
    .info-row strong {
      color: #e2e8f0;
    }
    .live-dot {
      display: inline-block;
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: #10b981;
      margin-right: 4px;
      animation: pulse 2s infinite;
    }
    @keyframes pulse {
      0% { opacity: 1; }
      50% { opacity: 0.4; }
      100% { opacity: 1; }
    }
  </style>
</head>
<body>
  <header>
    <h1>PocketJS 嵌入式模拟器</h1>
    <span class="badge"><span class="live-dot"></span>ESP32-S3 N16R8 · ${VIEWPORT_WIDTH}×${VIEWPORT_HEIGHT} (Vue Vapor)</span>
  </header>

  <div class="device-shell">
    <div class="device-screen-bezel">
      <canvas id="screen" width="${VIEWPORT_WIDTH}" height="${VIEWPORT_HEIGHT}"></canvas>
    </div>

    <div class="device-controls">
      <button id="btn-boot" class="btn">模拟触控点击 (Tap Hero)</button>
      <button id="btn-rst" class="btn" style="background:#1e293b;">RST 复位</button>
    </div>
  </div>

  <div class="info-panel">
    <div class="info-row"><span>运行状态</span><strong id="stat-status">加载中...</strong></div>
    <div class="info-row"><span>帧率 (FPS)</span><strong id="stat-fps">--</strong></div>
    <div class="info-row"><span>样式系统</span><strong style="color:#38bdf8;">Tailwind + Baked Fonts</strong></div>
    <div class="info-row"><span>热重载 (Live Reload)</span><strong style="color:#10b981;">已就绪 (监听 ui/src)</strong></div>
    <div class="info-row"><span>屏幕规格</span><strong>${VIEWPORT_WIDTH} × ${VIEWPORT_HEIGHT} (RGB565 映射)</strong></div>
    <div style="margin-top:8px; border-top:1px solid #1e293b; padding-top:6px; color:#64748b;">
      💡 提示：点击屏幕上的按钮或按下键盘 <strong>Enter / Z</strong> 均可与应用互动。
    </div>
  </div>

  <script type="module">
    window.process = { env: { NODE_ENV: "production" } };
    import { createWasmUi } from "/wasm-ops.js";

    const canvas = document.getElementById("screen");
    const ctx = canvas.getContext("2d");
    const statusEl = document.getElementById("stat-status");
    const fpsEl = document.getElementById("stat-fps");

    let wasm = null;
    let held = 0;
    let rafId = null;
    let last = performance.now();
    let acc = 0;
    let frames = 0;
    let secTimer = performance.now();

    const BTN_CIRCLE = 0x2000;

    async function init() {
      try {
        statusEl.textContent = "正在初始化 WebAssembly 核心...";
        const wasmRes = await fetch("/pocketjs.wasm");
        const wasmBytes = await wasmRes.arrayBuffer();

        wasm = await createWasmUi(wasmBytes, {
          width: ${VIEWPORT_WIDTH},
          height: ${VIEWPORT_HEIGHT},
          rasterDensity: 1
        });

        globalThis.ui = wasm.ops;
        globalThis.frame = undefined;

        // 加载包含 Tailwind 样式表与 baked 字体的 PAK 资产包
        statusEl.textContent = "加载字体与样式资产包...";
        const pakRes = await fetch("/dist/remapad-ui.pak?t=" + Date.now());
        if (pakRes.ok) {
          const pakBuf = await pakRes.arrayBuffer();
          globalThis.__pak = pakBuf;
        }

        statusEl.textContent = "加载应用代码 bundle...";
        const appRes = await fetch("/dist/remapad-ui.js?t=" + Date.now());
        const appCode = await appRes.text();
        new Function(appCode)();

        statusEl.textContent = "运行中 (60 FPS)";
        startLoop();
      } catch (err) {
        console.error(err);
        statusEl.textContent = "加载失败: " + err.message;
        statusEl.style.color = "#ef4444";
      }
    }

    function startLoop() {
      if (rafId) cancelAnimationFrame(rafId);
      const imageData = ctx.createImageData(${VIEWPORT_WIDTH}, ${VIEWPORT_HEIGHT});

      function tick(now) {
        rafId = requestAnimationFrame(tick);
        try {
          let dt = now - last;
          last = now;
          if (dt > 250) dt = 250;
          acc += dt;

          const STEP = 1000 / 60;
          while (acc >= STEP) {
            let frameTouches = [];
            for (const tap of touchQueue) {
              if (tap.stage === 0) {
                const packed = (tap.x & 511) | ((tap.y & 511) << 9) | (1 << 18);
                frameTouches.push(packed);
                tap.stage = 1;
              }
            }
            touchQueue = touchQueue.filter((t) => t.stage <= 1);

            if (typeof globalThis.frame === "function") {
              globalThis.frame(held, 0x8080, frameTouches);
            }

            for (const tap of touchQueue) {
              if (tap.stage === 1) tap.stage = 2;
            }
            wasm.tick();
            acc -= STEP;
            frames++;
          }

          const pixels = wasm.render();
          imageData.data.set(pixels);
          ctx.putImageData(imageData, 0, 0);

          if (now - secTimer >= 1000) {
            fpsEl.textContent = Math.round((frames * 1000) / (now - secTimer));
            frames = 0;
            secTimer = now;
          }
        } catch (err) {
          console.error("Loop error:", err);
          statusEl.textContent = "渲染错误: " + err.message;
          statusEl.style.color = "#ef4444";
          cancelAnimationFrame(rafId);
        }
      }

      last = performance.now();
      secTimer = performance.now();
      rafId = requestAnimationFrame(tick);
    }

    let touchQueue = [];

    function queueTap(x, y) {
      touchQueue.push({ x, y, stage: 0 });
    }

    document.getElementById("btn-boot").addEventListener("click", () => {
      queueTap(${Math.floor(VIEWPORT_WIDTH / 2)}, ${VIEWPORT_HEIGHT - 30});
    });

    canvas.addEventListener("pointerdown", (e) => {
      const rect = canvas.getBoundingClientRect();
      const x = Math.floor((e.clientX - rect.left) * (${VIEWPORT_WIDTH} / rect.width));
      const y = Math.floor((e.clientY - rect.top) * (${VIEWPORT_HEIGHT} / rect.height));
      queueTap(x, y);
    });

    document.getElementById("btn-rst").addEventListener("click", () => {
      location.reload();
    });

    window.addEventListener("keydown", (e) => {
      if (e.key === "Enter" || e.key === "z" || e.key === "Z") {
        held |= BTN_CIRCLE;
      }
    });

    window.addEventListener("keyup", (e) => {
      if (e.key === "Enter" || e.key === "z" || e.key === "Z") {
        held &= ~BTN_CIRCLE;
      }
    });

    const evt = new EventSource("/api/events");
    evt.onmessage = (e) => {
      if (e.data === "reload") {
        console.log("[PocketJS Preview] 监听到文件更新，重新加载应用...");
        location.reload();
      }
    };

    init();
  </script>
</body>
</html>
`;

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

  if (url.pathname === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(SIMULATOR_HTML);
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
