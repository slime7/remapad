// 从 PocketJS 官方 checkout 重新生成 ui/vendor/pocketjs 快照。
//
//   node scripts/vendor-pocketjs.mjs <PocketJS checkout 路径>
//
// 快照包含编译器入口的静态导入闭包、框架源码、契约定义、字体与图片资源，以及触摸预览
// 使用的官方 wasm 核心。依赖（vue、solid-js 等）不进快照，仍由 ui/package.json 提供。
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
const PROJECT_ROOT = resolve(SCRIPT_DIR, '..');
const VENDOR = resolve(PROJECT_ROOT, 'ui/vendor/pocketjs');
const checkout = resolve(process.argv[2] ?? resolve(PROJECT_ROOT, '../pocketjs'));

if (!existsSync(join(checkout, 'tools/pocket.ts'))) {
  console.error('未找到 PocketJS checkout: ' + checkout);
  console.error('用法: node scripts/vendor-pocketjs.mjs <PocketJS checkout 路径>');
  process.exit(1);
}

// 编译器生成的样式表镜像必须一起进快照：官方 CLI 的类型检查跑在编译器写入它之前，快照
// 缺少该文件时，新克隆的仓库连 pnpm run check 都过不去。先确认再删除旧快照。
if (!existsSync(join(checkout, 'framework/src/styles.generated.ts'))) {
  console.error('PocketJS checkout 缺少 framework/src/styles.generated.ts。');
  console.error('先在该 checkout 里执行官方 bun tools/build.ts 生成它，再重新生成本快照。');
  process.exit(1);
}

const pkg = JSON.parse(readFileSync(join(checkout, 'package.json'), 'utf8'));
const IMPORT_RE = /from\s*["']([^"']+)["']/g;

// 1. 编译器入口的静态导入闭包。
const seen = new Set();
const queue = ['tools/pocket.ts', 'tools/build.ts'].map((file) => join(checkout, file));
while (queue.length > 0) {
  const file = queue.pop();
  if (seen.has(file) || !existsSync(file)) {
    continue;
  }
  seen.add(file);
  const source = readFileSync(file, 'utf8');
  let match;
  while ((match = IMPORT_RE.exec(source))) {
    const spec = match[1];
    if (spec.startsWith('.')) {
      const base = resolve(dirname(file), spec);
      for (const candidate of [base, base + '.ts', base + '.tsx', base + '.json', join(base, 'index.ts')]) {
        if (existsSync(candidate) && statSync(candidate).isFile()) {
          queue.push(candidate);
          break;
        }
      }
    } else if (spec.startsWith('@pocketjs/framework')) {
      const target = pkg.exports['./' + spec.slice('@pocketjs/framework/'.length)];
      if (typeof target === 'string') {
        queue.push(join(checkout, target));
      }
    }
  }
}

rmSync(VENDOR, { recursive: true, force: true });
mkdirSync(VENDOR, { recursive: true });

// 2. 框架源码、契约、字体与图片资源整体复制。
for (const relative of ['framework', 'contracts', 'assets/fonts', 'assets/images']) {
  cpSync(join(checkout, relative), join(VENDOR, relative), { recursive: true });
}

// 3. 触摸预览使用的浏览器运行时，以及官方 DevTools 服务器与面板（serve.ts
//    依赖 server.ts，面板页 devtools.html 加载 devtools.js，均无外部依赖）。
mkdirSync(join(VENDOR, 'hosts/web'), { recursive: true });
for (const relative of [
  'hosts/web/wasm-ops.js',
  'hosts/web/pocketjs.wasm',
  'hosts/web/serve.ts',
  'hosts/web/server.ts',
  'hosts/web/devtools.html',
  'hosts/web/devtools.js',
]) {
  cpSync(join(checkout, relative), join(VENDOR, relative));
}

// 4. 编译器入口与其依赖。
for (const file of seen) {
  const relative = file.slice(checkout.length + 1).replace(/\\/g, '/');
  const target = join(VENDOR, relative);
  mkdirSync(dirname(target), { recursive: true });
  cpSync(file, target);
}

// 5. 只保留子路径解析需要的清单元数据。
writeFileSync(join(VENDOR, 'package.json'), JSON.stringify({
  name: pkg.name,
  version: pkg.version,
  license: pkg.license,
  type: pkg.type,
  private: true,
  exports: pkg.exports,
}, null, 2) + '\n');
cpSync(join(checkout, 'tsconfig.json'), join(VENDOR, 'tsconfig.json'));

console.log('已复制编译器文件 ' + seen.size + ' 个');
console.log('快照已生成: ' + VENDOR);
console.log('来源版本: ' + pkg.name + '@' + pkg.version);
