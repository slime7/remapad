#!/usr/bin/env node
// Remapad PC 侧统一入口：gui 图形控制台、ctrl 桥接命令行、mcp 按键注入服务。
// 发布包里的 remapad.cmd 也落到这里； gui --dev 走 vite 热更新，只在仓库里可用。
import process from 'node:process';

const [command, ...rest] = process.argv.slice(2);

async function gui() {
  if (rest.includes('--dev')) {
    const args = rest.filter((arg) => arg !== '--dev');
    const { main } = await import('./gui/dev.mjs');
    return main(args);
  }
  const { main } = await import('./src/gui-server/cli.js');
  return main(rest);
}

async function ctrl() {
  const { main } = await import('./src/ctrl.js');
  return main(rest);
}

async function mcp() {
  const { main } = await import('./src/mcp/cli.js');
  return main(rest);
}

const commands = { gui, ctrl, mcp };
const main = commands[command];
if (!main) {
  console.error('用法：remapad <gui|ctrl|mcp> [参数...]');
  process.exit(2);
}
main().then((code) => process.exit(code ?? 0), (exc) => {
  console.error(exc);
  process.exit(1);
});
