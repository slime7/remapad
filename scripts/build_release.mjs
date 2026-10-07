// 发布包构建：前端构建产物 + pc 源码与生产依赖 + Node 运行时，打成免安装的 zip。
// 产物在 pc/dist/，解压后命令行 `remapad.cmd <gui|ctrl|mcp>`，无需安装 Node。
import { execSync } from 'node:child_process';
import { cpSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = join(root, 'pc', 'dist');
const pkg = join(dist, 'app');
const { version } = JSON.parse(readFileSync(join(root, 'pc', 'package.json'), 'utf8'));

function run(command) {
  execSync(command, { cwd: root, stdio: 'inherit' });
}

rmSync(dist, { recursive: true, force: true });

run('pnpm --filter @remapad/gui build');
run(`pnpm --filter @remapad/pc deploy --prod --legacy "${pkg}"`);

// deploy 只拷 pc 包内容；前端构建产物与测试文件手动归位。
rmSync(join(pkg, 'test'), { recursive: true, force: true });
rmSync(join(pkg, 'vitest.config.js'), { force: true });
cpSync(join(root, 'pc', 'gui', 'dist'), join(pkg, 'gui', 'dist'), { recursive: true });

// Node 运行时取构建机正在用的这一份，包随构建机的平台与架构。
const nodeDir = join(pkg, 'node');
mkdirSync(nodeDir, { recursive: true });
cpSync(process.execPath, join(nodeDir, process.platform === 'win32' ? 'node.exe' : 'node'));

writeFileSync(
  join(dist, 'remapad.cmd'),
  '@echo off\r\n'
  + '"%~dp0app\\node\\node.exe" "%~dp0app\\remapad.mjs" %*\r\n',
);

const zip = join(dist, `remapad-${version}-win-x64.zip`);
run(`powershell -NoProfile -Command "Compress-Archive -Path '${join(dist, 'app')}','${join(dist, 'remapad.cmd')}'`
  + ` -DestinationPath '${zip}' -Force"`);
console.log(`发布包：${zip}`);
