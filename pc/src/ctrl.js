#!/usr/bin/env node
// Remapad PC 侧单工具：桥接转发 + 串口命令行 + 实机截图 + 固件 OTA + amiibo 上传。
// 设备只有一根 Type-C：USB-Serial/JTAG 同时承载桥接帧、固件日志与 CLI 文本。
// 用法与命令面见 pc/README.md；`:` 开头的是本工具命令（:help 看全表），
// 其余行按固件 CLI 原样发送。

import { openHint } from './link/serial.js';
import { UdpLink, parseEndpoint } from './link/net.js';
import { SerialLink } from './link/serial.js';
import { HidUnavailable, describe, listCandidates, listVirtualPads, loadHid, pickDevice } from './input/pads.js';
import { AmiiboError, ImageError, loadImage } from './session/image.js';
import { Session, expandUser, waitForVersion } from './session/session.js';
import { parseCtrlArgs } from './args.js';
import { now, sleep } from './util.js';

/** 命令行入口：环境与镜像类问题统一映射成退出码 2。 */
export async function main(argv = process.argv.slice(2)) {
  const args = parseCtrlArgs(argv);
  const command = args.command.filter((item) => item.length);
  try {
    return await run(args, command);
  } catch (exc) {
    if (exc instanceof HidUnavailable || exc instanceof ImageError || exc instanceof AmiiboError) {
      console.error(exc.message);
      return 2;
    }
    throw exc;
  }
}/** 端口打开失败的统一出口：提示一句原因并以环境问题（退出码 2）结束。 */
function openPortOrExit(port, baud) {
  try {
    return new SerialLink(port, baud);
  } catch (exc) {
    console.error(`${port}: ${openHint(exc)}`);
    process.exit(2);
  }
}

async function run(args, rawCommands) {
  // 老用法的 log 子命令：等价于 --log，参数照旧。
  let commands = rawCommands;
  if (commands.length && commands[0] === 'log') {
    args.log = true;
    const rest = commands.slice(1);
    if (rest.includes('--reset')) {
      args.reset = true;
    }
    if (rest.includes('--raw')) {
      args.raw = true;
    }
    const index = rest.indexOf('--seconds');
    if (index >= 0 && index + 1 < rest.length) {
      args.seconds = Number(rest[index + 1]);
    }
    commands = [];
  }

  if (args.dryRun) {
    const imagePath = expandUser(args.image);
    const { data: image, version } = loadImage(imagePath);
    console.log(`镜像 ${imagePath}：${image.length} 字节，版本 ${version}`);
    console.log('dry-run：镜像校验通过，未连接设备');
    return 0;
  }

  if (args.list) {
    return runList(loadHid());
  }
  if (args.dump) {
    return runDump(args, loadHid());
  }

  const hid = args.noPad ? null : loadHid();
  let conn;
  if (args.net) {
    const [host, port] = parseEndpoint(args.net);
    conn = new UdpLink(host, port);
    console.log(`网络会话 ${host}:${port}（UDP，netlog 通道）`);
  } else {
    conn = openPortOrExit(args.port, args.baud);
  }
  let code;
  const session = new Session(args, hid, conn);
  activeSession = session;
  try {
    if (args.upgrade) {
      code = await session.runUpgrade();
    } else if (args.amiibo) {
      code = await session.runAmiibo(args.amiibo);
    } else if (args.shot) {
      code = await session.runShot(args.out);
    } else if (args.all) {
      code = await session.runAll();
    } else if (args.log) {
      code = await session.runLog(args.seconds, args.reset, args.raw);
    } else if (args.capture) {
      code = await session.runCapture(args.capture, args.seconds);
    } else if (commands.length) {
      code = await session.runCommand(commands.join(' '));
    } else {
      code = await session.runInteractive();
    }
  } finally {
    session.stopCapture();
    await session.detachPad();
    activeSession = null;
    conn.close();
  }
  if (code !== 0 || !args.wait || !args.upgrade) {
    return code;
  }
  if (args.net) {
    console.error('--wait 只等串口：WiFi 升级后 netlog 会话随重启关闭，'
            + '请在设备上重开「无线调试」后再连');
  }
  return waitForVersion(args.port, args.baud);
}

/** Ctrl+C：让主循环自己收尾（发 DETACH、关端口）；再按一次立即退出。 */
let activeSession = null;
let interruptCount = 0;
process.on('SIGINT', () => {
  interruptCount += 1;
  if (activeSession === null || interruptCount >= 2) {
    process.exit(130);
  }
  activeSession.stop = true;
});

export async function runList(hid) {
  const infos = hid.enumerate();
  const candidates = listCandidates(infos);
  const virtual = listVirtualPads(infos);
  if (!candidates.length && !virtual.length) {
    console.log('没有找到手柄接口');
    return 1;
  }
  for (const info of candidates) {
    console.log(describe(info));
  }
  for (const info of virtual) {
    console.log(`${describe(info)}（虚拟手柄，不参与转发）`);
  }
  return 0;
}

export async function runDump(args, hid) {
  /** 只打印原始报告：用来核对固件家族表里的字段偏移与位序。 */
  const info = pickDevice(args, hid.enumerate());
  if (info === null) {
    if (!listCandidates(hid.enumerate()).length) {
      console.error('没有找到手柄接口（--list 可以看到全部候选）');
    } else {
      console.error('--vid/--pid 没有匹配到任何手柄接口（--list 可以看到全部候选）');
    }
    return 1;
  }
  console.log(`dump: ${describe(info)}`);
  const dev = await hid.open(info.path);
  const started = now();
  try {
    while (args.seconds <= 0 || now() - started < args.seconds) {
      const data = await dev.read();
      if (!data || !data.length) {
        await sleep(0.001);
        continue;
      }
      const raw = Buffer.from(data);
      const stamp = now() - started;
      const hex = [...raw].map((b) => b.toString(16).padStart(2, '0')).join(' ');
      console.log(`[${stamp.toFixed(3).padStart(7)}s] len=${String(raw.length).padStart(2)} ${hex}`);
    }
  } finally {
    await dev.close();
  }
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
