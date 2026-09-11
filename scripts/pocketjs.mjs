// 官方 PocketJS 工具链入口。解析 POCKETJS_ROOT 后，把本仓库的 manifest、host profile
// 和工程根目录交给 checkout 中的官方 CLI；web 预览使用官方 hosts/web 开发主机。
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = resolve(SCRIPT_DIR, '..');
const UI_ROOT = resolve(PROJECT_ROOT, 'ui');
const MANIFEST = resolve(UI_ROOT, 'pocket.json');
const HOST_PROFILE = resolve(PROJECT_ROOT, 'firmware/pocket.host.json');
const UI_OUTDIR = resolve(UI_ROOT, 'dist');
const PACKAGE_OUTPUT = resolve(UI_OUTDIR, 'remapad-ui.pocket');
const SIBLING_CHECKOUT = resolve(PROJECT_ROOT, '../pocketjs');
const INSTALLED_FRAMEWORK = resolve(UI_ROOT, 'node_modules/@pocketjs/framework');

const argv = process.argv.slice(2);
const command = argv.shift() ?? '';
const backendArgs = argv.filter((value) => value !== '--');

if (!['check', 'compile', 'build', 'web', 'native'].includes(command)) {
  console.error('usage: node scripts/pocketjs.mjs <check|compile|build|web|native>');
  process.exit(1);
}

function hasHostProfileCompiler(root) {
  const script = resolve(root, 'tools/pocket.ts');
  try {
    return existsSync(script) && readFileSync(script, 'utf8').includes('--host-profile');
  } catch {
    return false;
  }
}

const candidates = [
  process.env.POCKETJS_ROOT?.trim() ? resolve(process.env.POCKETJS_ROOT.trim()) : null,
  SIBLING_CHECKOUT,
  INSTALLED_FRAMEWORK,
].filter((root, index, roots) => root !== null && roots.indexOf(root) === index);

const FRAMEWORK_ROOT = candidates.find(hasHostProfileCompiler);
if (!FRAMEWORK_ROOT) {
  console.error(
    '[Remapad] 未找到包含 --host-profile 的 PocketJS compiler。请把 POCKETJS_ROOT 指向官方 checkout。',
  );
  process.exit(1);
}

function run(args) {
  const result = spawnSync('bun', args, { cwd: FRAMEWORK_ROOT, stdio: 'inherit' });
  if (result.error) {
    console.error('[Remapad] 无法启动 PocketJS 官方脚本:', result.error.message);
    process.exit(1);
  }
  return result.status ?? 1;
}

function cliArgs(subcommand, outdir) {
  const args = [
    resolve(FRAMEWORK_ROOT, 'tools/pocket.ts'),
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

if (command === 'native') {
  const cargo = process.env.POCKETJS_CARGO?.trim();
  const args = [
    resolve(FRAMEWORK_ROOT, 'tools/esp-idf-native.ts'),
    '--target',
    'esp32s3',
    ...(cargo ? ['--cargo', cargo] : []),
    ...backendArgs,
  ];
  process.exit(run(args));
}

if (command === 'web') {
  // 官方 Web 开发主机只从 PocketJS checkout 的 dist/ 读取 bundle，因此编译产物
  // 直接写入该目录，再由 hosts/web/serve.ts 提供预览页面。
  const distDir = resolve(FRAMEWORK_ROOT, 'dist');
  const status = run(cliArgs('compile', distDir));
  if (status !== 0) {
    process.exit(status);
  }
  // 官方主机首次运行需要 engine/wasm 编译出的 pocketjs.wasm；它由官方脚本生成，
  // 与 bundle 无关，因此只在缺失时构建一次。
  const wasmFile = resolve(FRAMEWORK_ROOT, 'hosts/web/pocketjs.wasm');
  if (!existsSync(wasmFile)) {
    console.log('[Remapad] 缺少官方 Web 主机的 wasm 核心，先执行 bun tools/wasm.ts');
    const wasmStatus = run([resolve(FRAMEWORK_ROOT, 'tools/wasm.ts')]);
    if (wasmStatus !== 0) {
      process.exit(wasmStatus);
    }
  }
  console.log('[Remapad] 官方 Web 预览: http://127.0.0.1:8130/?demo=remapad-ui&width=240&height=280&density=1');
  process.exit(run([resolve(FRAMEWORK_ROOT, 'hosts/web/serve.ts')]));
}

process.exit(run(cliArgs(command, UI_OUTDIR)));
