// 官方 PocketJS 工具链入口。编译器、框架源码、字体与浏览器运行时都取自仓库内的
// ui/vendor/pocketjs 快照，因此项目自身就能完成检查、编译、打包与预览；POCKETJS_ROOT
// 只在需要对照官方 checkout 或重建原生归档时使用。
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, readFileSync, symlinkSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = resolve(SCRIPT_DIR, '..');
const UI_ROOT = resolve(PROJECT_ROOT, 'ui');
const MANIFEST = resolve(UI_ROOT, 'pocket.json');
const HOST_PROFILE = resolve(PROJECT_ROOT, 'firmware/pocket.host.json');
const UI_OUTDIR = resolve(UI_ROOT, 'dist');
const PACKAGE_OUTPUT = resolve(UI_OUTDIR, 'remapad-ui.pocket');
const COMPONENTS_DIR = resolve(PROJECT_ROOT, 'firmware/components');
const VENDOR_ROOT = resolve(UI_ROOT, 'vendor/pocketjs');
const SIBLING_CHECKOUT = resolve(PROJECT_ROOT, '../pocketjs');

const argv = process.argv.slice(2);
const command = argv.shift() ?? '';
const backendArgs = argv.filter((value) => value !== '--');

if (!['check', 'compile', 'build', 'web', 'native'].includes(command)) {
  console.error('usage: node scripts/pocketjs.mjs <check|compile|build|web|native>');
  process.exit(1);
}

const candidates = [
  process.env.POCKETJS_ROOT?.trim() ? resolve(process.env.POCKETJS_ROOT.trim()) : null,
  VENDOR_ROOT,
  SIBLING_CHECKOUT,
].filter((root, index, roots) => root !== null && roots.indexOf(root) === index);

function requireRoot(predicate, requirement) {
  const found = candidates.find(predicate);
  if (!found) {
    console.error(`[Remapad] 找不到 ${requirement}。`);
    console.error('[Remapad] 先执行 pnpm install；若要用官方源码 checkout，请把 POCKETJS_ROOT 指向它。');
    process.exit(1);
  }
  return found;
}

function hasHostProfileCompiler(root) {
  const script = resolve(root, 'tools/pocket.ts');
  try {
    return existsSync(script) && readFileSync(script, 'utf8').includes('--host-profile');
  } catch {
    return false;
  }
}

const hasNativeBuilder = (root) => existsSync(resolve(root, 'tools/esp-idf-native.ts'));
const hasWebHost = (root) => existsSync(resolve(root, 'hosts/web/serve.ts'));

function run(args, cwd) {
  const result = spawnSync('bun', args, { cwd, stdio: 'inherit' });
  if (result.error) {
    console.error('[Remapad] 无法启动 PocketJS 官方脚本:', result.error.message);
    process.exit(1);
  }
  return result.status ?? 1;
}

function cliArgs(root, subcommand, outdir) {
  const args = [
    resolve(root, 'tools/pocket.ts'),
    subcommand,
    '--host-profile',
    HOST_PROFILE,
    '--manifest',
    MANIFEST,
    '--project-root',
    UI_ROOT,
    '--outdir',
    outdir,
    ...backendArgs,
  ];
  if (subcommand === 'build') {
    args.push('--output', PACKAGE_OUTPUT);
  }
  return args;
}

// 编译器在打包阶段从自身包根解析 vue、solid-js 等运行时依赖。快照位于仓库内，
// 因此这里把它指向项目已安装的 ui/node_modules，避免同一批依赖出现第二份副本。
// 目标已存在时不做任何事；pnpm install 会重建 ui/node_modules。
function ensureVendorNodeModules() {
  const link = resolve(VENDOR_ROOT, 'node_modules');
  if (existsSync(link)) {
    return;
  }
  const target = resolve(UI_ROOT, 'node_modules');
  if (!existsSync(target)) {
    console.error('[Remapad] 缺少 ' + target + '，请先执行 pnpm install');
    process.exit(1);
  }
  symlinkSync(target, link, process.platform === 'win32' ? 'junction' : 'dir');
  console.log('[Remapad] 已把 ui/vendor/pocketjs/node_modules 指向项目依赖目录');
}

// 框架源码 import 了编译器生成的 framework/src/styles.generated.ts，而官方 CLI 的类型检查
// 跑在编译器写入该文件之前，所以它必须随快照提交、并在 pnpm install 之前就位：安装之后再
// 写入的文件不会进入 pnpm 的依赖副本，类型检查仍然会以 TS2307 失败。缺失时给出修复提示，
// 具体错误仍由官方 CLI 输出。
const GENERATED_STYLES = 'framework/src/styles.generated.ts';

function warnIfGeneratedStylesMissing(root) {
  if (existsSync(resolve(root, GENERATED_STYLES))) {
    return;
  }
  console.error('[Remapad] ' + root + ' 缺少 ' + GENERATED_STYLES + '，请从 git 恢复该文件，或在该 checkout 里执行官方 bun tools/build.ts 后重新执行 pnpm install');
}

/** 监听 UI 源码，变更后重新编译并让预览页加载新产物。 */
async function watchUiSources(compilerRoot, server) {
  const { readdirSync, statSync, watch } = await import('node:fs');
  const watched = [resolve(UI_ROOT, 'src'), MANIFEST, HOST_PROFILE];
  let compiling = false;
  let pending = false;
  let timer = null;
  let lastSignature = '';

  // 目录监听对同一次写入可能产生多个事件，用「最新修改时间」指纹去重；
  // 官方编译器会在入口目录里创建并删除 .pocketjs-app-check-* 临时目录，
  // 这类点开头的条目必须排除，否则监听会被自己的构建过程反复触发。
  const signature = () => {
    const parts = [];
    const walk = (directory) => {
      let entries;
      try {
        entries = readdirSync(directory, { withFileTypes: true });
      } catch {
        return;
      }
      for (const entry of entries) {
        if (entry.name.startsWith('.')) {
          continue;
        }
        const full = resolve(directory, entry.name);
        if (entry.isDirectory()) {
          walk(full);
        } else {
          try {
            parts.push(full + ':' + statSync(full).mtimeMs + ':' + statSync(full).size);
          } catch {
            // 文件在读取过程中消失，忽略即可。
          }
        }
      }
    };
    walk(resolve(UI_ROOT, 'src'));
    for (const file of [MANIFEST, HOST_PROFILE]) {
      try {
        parts.push(file + ':' + statSync(file).mtimeMs + ':' + statSync(file).size);
      } catch {
        // 文件缺失时由编译阶段报错。
      }
    }
    return parts.sort().join('|');
  };

  const compile = () => {
    if (compiling) {
      pending = true;
      return;
    }
    compiling = true;
    const child = spawn('bun', cliArgs(compilerRoot, 'compile', UI_OUTDIR), {
      cwd: compilerRoot,
      stdio: ['ignore', 'ignore', 'inherit'],
    });
    // spawn 失败（bun 丢失/被占用等）只触发 error 不触发 close；挂上处理器避免
    // 未捕获的 error 事件带崩整个 dev 进程。
    child.on('error', (error) => {
      compiling = false;
      console.error('[Remapad] 重新编译进程启动失败: ' + error.message);
      if (pending) {
        pending = false;
        compile();
      }
    });
    child.on('close', (code) => {
      compiling = false;
      if (code === 0) {
        console.log('[Remapad] 已重新编译，通知预览页加载新产物');
        server.notifyReload();
      } else {
        console.error('[Remapad] 重新编译失败，预览页继续使用上一份产物（退出码 ' + code + '）');
      }
      if (pending) {
        pending = false;
        compile();
      }
    });
  };

  for (const target of watched) {
    try {
      watch(target, { recursive: true }, () => {
        if (timer) {
          clearTimeout(timer);
        }
        timer = setTimeout(() => {
          const current = signature();
          if (current === lastSignature) {
            return;
          }
          lastSignature = current;
          compile();
        }, 200);
      });
    } catch (error) {
      console.error('[Remapad] 无法监听 ' + target + '：' + error.message);
    }
  }
}

if (command === 'native') {
  // 原生 Rust 归档由官方脚本从 PocketJS 源码构建，产物直接写入本仓库的组件目录。
  const builderRoot = requireRoot(hasNativeBuilder, 'tools/esp-idf-native.ts');
  const cargo = process.env.POCKETJS_CARGO?.trim();
  console.log('[Remapad] 生成 ESP32-S3 原生归档，需要固定版本的 Xtensa Rust（esp-rs/rust-build v1.97.0.0）');
  process.exit(run([
    resolve(builderRoot, 'tools/esp-idf-native.ts'),
    '--target',
    'esp32s3',
    '--output-root',
    COMPONENTS_DIR,
    ...(cargo ? ['--cargo', cargo] : []),
    ...backendArgs,
  ], builderRoot));
}

// 编译需要含 ESP-IDF host profile 的官方 compiler；预览主机可以与编译分离，这样
// bundle 仍然输出到本仓库的 ui/dist。
const compilerRoot = requireRoot(hasHostProfileCompiler, '包含 --host-profile 的 PocketJS compiler');

if (compilerRoot === VENDOR_ROOT) {
  ensureVendorNodeModules();
}
warnIfGeneratedStylesMissing(compilerRoot);
console.log('[Remapad] compiler: ' + compilerRoot);

if (command === 'web') {
  // 浏览器运行时取自与编译器同一份快照，避免版本错配。
  const runtimeRoot = [VENDOR_ROOT, ...candidates].find(hasWebHost) ?? compilerRoot;
  const runtimeDir = resolve(runtimeRoot, 'hosts/web');
  console.log('[Remapad] compiler: ' + compilerRoot);
  console.log('[Remapad] 浏览器运行时: ' + runtimeDir);
  const status = run(cliArgs(compilerRoot, 'compile', UI_OUTDIR), compilerRoot);
  if (status !== 0) {
    process.exit(status);
  }
  if (!existsSync(resolve(runtimeDir, 'pocketjs.wasm'))) {
    console.log('[Remapad] 缺少官方 wasm 核心，先执行 bun tools/wasm.ts（需要 rustup target add wasm32-unknown-unknown）');
    const wasmStatus = run([resolve(runtimeRoot, 'tools/wasm.ts')], runtimeRoot);
    if (wasmStatus !== 0) {
      process.exit(wasmStatus);
    }
  }
  const { startPreviewServer } = await import('./preview-server.mjs');
  const server = await startPreviewServer({
    pageDir: resolve(PROJECT_ROOT, 'ui/preview'),
    distDir: UI_OUTDIR,
    runtimeDir,
  });
  console.log('[Remapad] 触摸屏预览: ' + server.url + '（240 × 280，触摸输入，无实体按键）');
  // 官方 DevTools 服务器（hosts/web/serve.ts = 面板 + WebSocket hub，单进程）。
  // 预览页以 device 角色接入它的 /ws，面板在 http://127.0.0.1:8131/devtools。
  const serveScript = resolve(runtimeDir, 'serve.ts');
  if (existsSync(serveScript)) {
    const devtools = spawn('bun', [serveScript], {
      cwd: runtimeDir,
      env: { ...process.env, PORT: '8131' },
      stdio: ['ignore', 'inherit', 'inherit'],
    });
    process.on('exit', () => {
      devtools.kill();
    });
    console.log('[Remapad] 官方 DevTools: http://127.0.0.1:8131/devtools');
  } else {
    console.log('[Remapad] 快照缺少 hosts/web/serve.ts，未启动官方 DevTools 服务器（重新执行 scripts/vendor-pocketjs.mjs 同步）');
  }
  watchUiSources(compilerRoot, server);
}

// check / compile / build 直接转发官方 CLI，产物写入本仓库的 ui/dist。
if (command !== 'web') {
  process.exit(run(cliArgs(compilerRoot, command, UI_OUTDIR), compilerRoot));
}
