// WASM 预览页的静态服务器：serve ui/preview/dist，.wasm 给对 MIME。
// --watch 时监听界面源码，变更即重编 dist，经 SSE（/__reload）通知浏览器整页刷新，
// 构建失败把日志尾推给页面浮层；监听面是喂给 wasm 构建的源码，不含用例与本目录脚本。
// 用法：node tools/serve.mjs [--port 8123] [--dist <目录>] [--watch]；端到端用例的 webServer 走不带 --watch 的形态。
import { spawn } from "node:child_process";
import { watch, readdirSync, statSync } from "node:fs";
import { readFile, stat } from "node:fs/promises";
import { createServer } from "node:http";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

const wasmDir = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const uiDir = path.resolve(wasmDir, "..");

// 界面源码（.slint 与 wasm 包）、编译口径与烘进二进制的字体底图都在监听面里。
const WATCH_ROOTS = [
  "src",
  "assets",
  "build-support",
  "preview/src",
  "preview/wasm.slint",
  "preview/preview-core.slint",
  "preview/index.html",
  "preview/build.rs",
  "preview/Cargo.toml",
].map((entry) => path.join(uiDir, entry));

function parseArgs(argv) {
  const args = { port: 8123, dist: path.join(wasmDir, "dist"), watch: false };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--port") {
      args.port = Number(argv[++i]);
    } else if (argv[i] === "--dist") {
      args.dist = path.resolve(argv[++i]);
    } else if (argv[i] === "--watch") {
      args.watch = true;
    }
  }
  return args;
}

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript",
  ".mjs": "text/javascript",
  ".wasm": "application/wasm",
  ".json": "application/json",
};

const args = parseArgs(process.argv.slice(2));

// ---- 热更新：SSE 客户端登记与广播 ----
const reloadClients = new Set();

function broadcast(event) {
  const payload = `data: ${JSON.stringify(event)}\n\n`;
  for (const response of reloadClients) {
    response.write(payload);
  }
}

// ---- 热更新：重编编排。构建期间再来的变更只记一笔，收尾后补一轮 ----
const build = { running: false, rerun: false, timer: null };

function scheduleBuild(delay = 150) {
  clearTimeout(build.timer);
  build.timer = setTimeout(runBuild, delay);
}

function runBuild() {
  if (build.running) {
    build.rerun = true;
    return;
  }
  build.running = true;
  console.log("[ui-wasm] 源码变更，重编 dist …");
  broadcast({ type: "building" });
  const child = spawn(process.execPath, [path.join(wasmDir, "tools", "build.mjs")]);
  let output = "";
  child.stdout.on("data", (chunk) => {
    output += chunk;
  });
  child.stderr.on("data", (chunk) => {
    output += chunk;
  });
  child.on("close", (code) => {
    build.running = false;
    if (code === 0) {
      console.log("[ui-wasm] 重编完成，已通知浏览器刷新");
      broadcast({ type: "reload" });
    } else {
      console.error(`[ui-wasm] 构建失败：\n${output}`);
      broadcast({ type: "error", message: output.slice(-4000) });
    }
    if (build.rerun) {
      build.rerun = false;
      scheduleBuild();
    }
  });
}

/** 给监听面布 watch：目录尽量 recursive，平台不支持（Linux）时退化为逐层平面监听。 */
function watchSources(onEvent) {
  const watchers = [];
  const armDir = (dir) => {
    try {
      watchers.push(watch(dir, { recursive: true }, onEvent));
    } catch {
      watchers.push(watch(dir, onEvent));
      for (const entry of readdirSync(dir, { withFileTypes: true })) {
        if (entry.isDirectory()) {
          armDir(path.join(dir, entry.name));
        }
      }
    }
  };
  for (const root of WATCH_ROOTS) {
    if (statSync(root).isDirectory()) {
      armDir(root);
    } else {
      watchers.push(watch(root, onEvent));
    }
  }
  return () => {
    for (const watcher of watchers) {
      watcher.close();
    }
  };
}

if (args.watch) {
  watchSources(() => scheduleBuild());
  if (!statSync(path.join(args.dist, "index.html"), { throwIfNoEntry: false })?.isFile()) {
    console.log("[ui-wasm] dist 还没有产物，先构建一轮 …");
    scheduleBuild(0);
  }
}

const server = createServer(async (request, response) => {
  const url = new URL(request.url, "http://127.0.0.1");
  if (url.pathname === "/__reload") {
    response.writeHead(200, {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-store",
      Connection: "keep-alive",
    });
    response.write("retry: 1000\n\n");
    reloadClients.add(response);
    request.on("close", () => reloadClients.delete(response));
    return;
  }
  const relative = decodeURIComponent(url.pathname).replace(/^\/+/, "");
  const candidate = path.resolve(args.dist, relative || "index.html");
  if (!candidate.startsWith(args.dist)) {
    response.writeHead(403).end();
    return;
  }
  try {
    const target = (await stat(candidate)).isDirectory() ? path.join(candidate, "index.html") : candidate;
    const body = await readFile(target);
    response.writeHead(200, {
      "Content-Type": MIME[path.extname(target)] ?? "application/octet-stream",
      "Cache-Control": "no-store",
    });
    response.end(body);
  } catch {
    response.writeHead(404).end("not found");
  }
});

server.listen(args.port, "127.0.0.1", () => {
  console.log(`[ui-wasm] 预览页：http://127.0.0.1:${server.address().port}/（Ctrl+C 退出）`);
});
