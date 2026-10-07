#!/usr/bin/env node
// Remapad 连接控制台入口：起 HTTP（REST + 静态前端）与 WS 推流，浏览器打开页面操作。
// 前端源码与构建见 pc/gui/；串口仍然只有一个持有者，别和 ctrl/MCP 同时连同一个口。
import process from 'node:process';

import { Command } from 'commander';

import { createGuiApp, waitForExit } from './app.js';
import { defaultDistDir } from './server.js';

function parseGuiArgs(argv = process.argv.slice(2)) {
  const program = new Command();
  program
    .name('remapad-gui')
    .description('Remapad 连接控制台（浏览器界面）')
    .option('--host <host>', 'HTTP 监听地址（默认只听本机）', '127.0.0.1')
    .option('--http-port <n>', 'HTTP 监听端口', (v) => Number(v), 8787)
    .option('--dist <dir>', '前端构建产物目录', defaultDistDir())
    .parse(argv, { from: 'user' });
  return program.opts();
}

export async function main(argv = process.argv.slice(2)) {
  const args = parseGuiArgs(argv);
  const { server, gui } = createGuiApp(args.dist);
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(args.httpPort, args.host, resolve);
  });
  console.log(`Remapad 连接控制台：http://${args.host}:${args.httpPort}/`);
  console.log('关闭此进程即断开会话；串口同一时间只允许一个持有者。');
  gui.refreshPorts().catch(() => {});
  gui.refreshPads().catch(() => {});
  await waitForExit(server, gui);
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
