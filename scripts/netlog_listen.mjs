#!/usr/bin/env node
// 局域网日志收听端：与固件 netlog 命令配套的 UDP 终端（临时调试工具）。
// 复用 pc/src/link 的桥接帧解码器与脱敏，链路与协议实现不在脚本里复刻。
// 用法：node scripts/netlog_listen.mjs [--port 9999] [--ip 设备IP]
// 设备侧 netlog 连上 AP 后日志与 CLI 回复逐行打印，键入一行回车即发往设备；
// 设备不知道 PC 的 IP：先向子网广播 hello（ping）被设备学到，--ip 指定点对点目标。
import { createSocket } from 'node:dgram';
import { networkInterfaces } from 'node:os';
import process from 'node:process';
import { createInterface } from 'node:readline';

import { FrameDecoder } from '../pc/src/link/frame.js';
import { NETLOG_PORT_DEFAULT } from '../pc/src/link/net.js';
import { maskSecrets } from '../pc/src/link/secrets.js';

function localBroadcastAddrs() {
  // 本机各 IPv4 网卡推导的 /24 广播地址（127.0.0.1 除外）。
  const addrs = new Set();
  for (const list of Object.values(networkInterfaces())) {
    for (const info of list ?? []) {
      if (info.family === 'IPv4' && info.address !== '127.0.0.1') {
        addrs.add(`${info.address.split('.').slice(0, 3).join('.')}.255`);
      }
    }
  }
  return addrs;
}

function stamp() {
  const now = new Date();
  const pad = (value) => String(value).padStart(2, '0');
  return `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
}

function parseArgs(argv = process.argv.slice(2)) {
  // 三个简单开关手写解析：scripts/ 不在 pnpm workspace 里，引 commander 反而解析不到。
  const args = { port: NETLOG_PORT_DEFAULT, ip: null, hello: true };
  for (let index = 0; index < argv.length; index += 1) {
    const token = argv[index];
    if (token === '--port') {
      args.port = Number(argv[++index]);
    } else if (token.startsWith('--port=')) {
      args.port = Number(token.slice('--port='.length));
    } else if (token === '--ip') {
      args.ip = argv[++index];
    } else if (token.startsWith('--ip=')) {
      args.ip = token.slice('--ip='.length);
    } else if (token === '--no-hello') {
      args.hello = false;
    } else if (token === '--help' || token === '-h') {
      console.log('用法：node scripts/netlog_listen.mjs [--port 9999] [--ip 设备IP] [--no-hello]');
      process.exit(0);
    } else {
      throw new Error(`不认识的参数：${token}`);
    }
  }
  if (!Number.isInteger(args.port) || args.port < 1 || args.port > 65535) {
    throw new Error(`--port 要是 1-65535 的整数，现在是 ${args.port}`);
  }
  return args;
}

export async function main(argv = process.argv.slice(2)) {
  const args = parseArgs(argv);
  const socket = createSocket({ type: 'udp4', reuseAddr: true });
  let peer = args.ip ?? null;
  const decoder = new FrameDecoder();

  socket.on('message', (data, info) => {
    if (data.equals(Buffer.from('ping\n'))) {
      return; // 本机 hello 广播的回环，别把目标学成自己
    }
    peer = info.address;
    if (data[0] === 0xa5 && data[1] === 0x5a) {
      const { frames } = decoder.feed(data);
      for (const [frameType, slot, , payload] of frames) {
        console.log(`[${stamp()}] <帧 0x${frameType.toString(16).padStart(2, '0')} slot=${slot} ${payload.length}B`);
      }
      return;
    }
    const text = data.toString('utf8').replace(/[\r\n]+$/, '');
    if (text) {
      console.log(`[${stamp()}] ${maskSecrets(text)}`);
    }
  });
  socket.on('error', (exc) => {
    console.error(`udp 出错：${exc.message}`);
  });
  await new Promise((resolve, reject) => {
    socket.once('error', reject);
    socket.bind(args.port, '0.0.0.0', resolve);
  });
  socket.setBroadcast(true);

  if (args.hello) {
    // 设备在等 PC 先说话才学得到目标：向子网广播（与 --ip）周期发 hello。
    const targets = new Set(args.ip ? [[args.ip, args.port]] : []);
    for (const broadcast of localBroadcastAddrs()) {
      targets.add([broadcast, args.port]);
    }
    if (targets.size > 0) {
      const hello = setInterval(() => {
        if (peer !== null) {
          clearInterval(hello);
          return;
        }
        for (const [host, port] of targets) {
          socket.send(Buffer.from('ping\n'), port, host, () => {});
        }
      }, 1000);
      hello.unref();
    }
  }

  console.log(`listening on udp/0.0.0.0:${args.port}，等设备 netlog 连上来`
        + `（${args.ip ?? '广播'} hello ${args.hello ? '已开启' : '已关闭'}）…`);
  const readline = createInterface({ input: process.stdin, terminal: true });
  for await (const line of readline) {
    if (peer === null) {
      console.log('还没收到设备的数据包：确认设备已连上 WiFi（netlog 状态）');
      continue;
    }
    socket.send(Buffer.from(`${line}\n`), args.port, peer);
  }
  socket.close();
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
