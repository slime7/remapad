#!/usr/bin/env node
// Remapad 按键注入 MCP 服务入口（stdio）：启动后不碰设备，连接由 remapad_connect 完成。
import { parseCtrlArgs } from '../args.js';
import { Command } from 'commander';
import { PadBridge } from './bridge.js';
import { DEFAULT_REPLAY_MAX_MS, DEFAULT_SCRIPT_MAX_MS } from './engine.js';
import { runStdio } from './server.js';

function parseMcpArgs(argv) {
  const program = new Command();
  program
    .name('remapad-mcp')
    .description('Remapad 按键注入 MCP 服务（stdio）')
    .option('-p, --port <port>', '默认串口名（remapad_connect 无参时使用；连接动作本身由工具完成）')
    .option('-n, --net <host[:port]>', '默认走设备 netlog 的 UDP 通道（WiFi）；设备侧 netlog 会话需开启')
    .option('--baud <n>', '波特率（USJ 忽略）', (v) => Number(v), 115200)
    .option('--reply-wait <s>', 'CLI 回复窗秒数', (v) => Number(v), 1.2)
    .option('--ping-ms <n>', 'WiFi 通道的 PING 保活间隔毫秒（维持设备侧 2 秒桥接窗口）', (v) => Number(v), 1000)
    .option('--script-max-ms <n>', '脚本总时长上限毫秒', (v) => Number(v), DEFAULT_SCRIPT_MAX_MS)
    .option('--replay-max-ms <n>', '回放总时长上限毫秒', (v) => Number(v), DEFAULT_REPLAY_MAX_MS)
    .parse(argv, { from: 'user' });
  return program.opts();
}

export async function main(argv = process.argv.slice(2)) {
  const args = parseMcpArgs(argv);
  if (args.port && args.net) {
    console.error('-p 与 -n 二选一');
    return 2;
  }
  const ctrlArgs = parseCtrlArgs(['--no-pad']);
  ctrlArgs.port = args.port ?? null;
  ctrlArgs.net = args.net ?? null;
  ctrlArgs.baud = args.baud;
  ctrlArgs.replyWait = args.replyWait;
  const bridge = new PadBridge(
    ctrlArgs,
    args.scriptMaxMs,
    args.replayMaxMs,
    args.pingMs / 1000.0,
  );
  const cleanup = () => {
    bridge.stop();
  };
  process.on('exit', cleanup);
  await runStdio(bridge);
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
