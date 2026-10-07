// ctrl 命令行参数解析：命令行、图形界面与 MCP 共用同一套参数面。
import { Command } from 'commander';

import { fileURLToPath } from 'node:url';

/** 默认镜像路径按包位置解析，因此从仓库任意目录运行都指向 firmware/build/remapad_firmware.bin。 */
export const DEFAULT_IMAGE = fileURLToPath(new URL('../../firmware/build/remapad_firmware.bin', import.meta.url));

function int0(value) {
  const parsed = Number(value);
  if (!Number.isInteger(parsed)) {
    throw new Error(`不是整数：${value}`);
  }
  return parsed;
}

function float(value) {
  const parsed = Number(value);
  if (!Number.isFinite(parsed)) {
    throw new Error(`不是数值：${value}`);
  }
  return parsed;
}

/** 解析 ctrl 的命令行参数，返回 camelCase 字段的普通对象（界面与 MCP 只取默认值）。 */
export function parseCtrlArgs(argv = []) {
  const program = new Command();
  program
    .name('ctrl')
    .description('Remapad PC 侧单工具：桥接转发 + 串口命令行 + 实机截图 + OTA')
    .option('-p, --port <port>', '串口名', 'COM3')
    .option('-n, --net <host[:port]>', '走设备 netlog 的 UDP 通道（WiFi）而不是串口')
    .option('--baud <n>', '波特率（USJ 忽略）', (v) => Number(v), 115200)
    .option('--vid <id>', '只挑该厂商 ID', int0)
    .option('--pid <id>', '只挑该产品 ID', int0)
    .option('--max-rate <hz>', '转发上限帧率（0 表示不限制，默认 250）', float, 250.0)
    .option('--no-rumble', '不把主机的震动/玩家灯写回手柄')
    .option('--no-audio-haptics', 'DS5 桥接时不走 PC 侧音频触觉（回落 HID 震动写回）')
    .option('--bt-haptics', '蓝牙接入的 DS5 启用私有触觉流（默认已启用，保留作显式声明）')
    .option('--no-bt-haptics', '蓝牙接入的 DS5 不用私有触觉流，回落 0x31 HID 两带震动')
    .option('--no-pad', '任何模式都不转发手柄（只用命令行 / 截图 / 日志 / 升级）')
    .option('--pad', '一次性命令 / 截图 / 日志 / 升级模式里也转发手柄')
    .option('--logs', '同时打印设备日志文本')
    .option('--reply-wait <s>', '一次性命令等回复的秒数', float, 1.2)
    .option('--list', '列出候选手柄接口后退出')
    .option('--dump', '只打印原始报告，不接串口')
    .option('--seconds <s>', '--dump / --log 的时长（0 表示到 Ctrl+C）', float, 0.0)
    .option('--raw', '--log 不加相对时间前缀')
    .option('--reset', '--log 先复位设备')
    .option('--shot', '抓实机截图后退出')
    .option('--out <path>', '截图输出路径')
    .option('--shot-timeout <s>', '等一次完整截图的秒数', float, 10.0)
    .option('--log', '只读设备日志（--seconds 控制时长）')
    .option('--capture <file>', '抓主机原始输出到文件后退出')
    .option('--all', '拉取设备全部观测数据后退出')
    .option('--upgrade', '推固件镜像后重启设备')
    .option('--image <path>', '镜像路径', DEFAULT_IMAGE)
    .option('--amiibo <bin>', '上传 amiibo 镜像到设备后退出')
    .option('--dry-run', '只校验镜像，不接设备')
    .option('--wait', '--upgrade 后等设备重启回来并打印版本')
    .option('--verbose', '升级时透传设备日志')
    .argument('[commands...]', '固件 CLI 命令（如 status），给了就执行一次后退出')
    .parse(argv, { from: 'user' });
  const opts = program.opts();
  return {
    port: opts.port,
    net: opts.net ?? null,
    baud: opts.baud,
    vid: opts.vid ?? null,
    pid: opts.pid ?? null,
    maxRate: opts.maxRate,
    noRumble: opts.noRumble ?? false,
    noAudioHaptics: opts.noAudioHaptics ?? false,
    btHaptics: opts.btHaptics,
    noPad: opts.noPad ?? false,
    pad: opts.pad ?? false,
    logs: opts.logs ?? false,
    replyWait: opts.replyWait,
    list: opts.list ?? false,
    dump: opts.dump ?? false,
    seconds: opts.seconds,
    raw: opts.raw ?? false,
    reset: opts.reset ?? false,
    shot: opts.shot ?? false,
    out: opts.out ?? null,
    shotTimeout: opts.shotTimeout,
    log: opts.log ?? false,
    capture: opts.capture ?? null,
    all: opts.all ?? false,
    upgrade: opts.upgrade ?? false,
    image: opts.image,
    amiibo: opts.amiibo ?? null,
    dryRun: opts.dryRun ?? false,
    wait: opts.wait ?? false,
    verbose: opts.verbose ?? false,
    command: program.args ?? [],
    padPath: null,
  };
}

/** 蓝牙接入的 DualSense 是否启用私有触觉流（0x32/0x36）。
 *
 * 默认启用：DS5 的 HD 触觉与手柄喇叭只有这条流承载，0x31 的 HID 写回只剩两带
 * 震动。--no-bt-haptics 显式关掉回落 HID 写回；写回被驱动拒绝时也会自动回落。
 * commander 把 --bt-haptics / --no-bt-haptics 合成一个布尔：缺省 undefined。
 */
export function btHapticsWanted(args) {
  return args.btHaptics !== false;
}
