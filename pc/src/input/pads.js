// 手柄候选的枚举、过滤与描述：转发与 --dump 共用同一份挑选规则。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { createRequire } from 'node:module';

import { CONN_BT, CONN_UNKNOWN, CONN_USB, FAMILY_NAMES, deviceId, familyForVendor } from '../link/frame.js';
import { connOfHidPath, isVirtualPad } from './devtree.js';

const require = createRequire(import.meta.url);

/** 手柄类接口：Generic Desktop / Joystick 与 Game Pad。 */
export const GAMEPAD_USAGE_PAGE = 0x01;
export const GAMEPAD_USAGES = [0x04, 0x05];

export const CONN_NAMES = { [CONN_UNKNOWN]: '-', [CONN_USB]: 'usb', [CONN_BT]: 'bt' };

/** hidapi 缺失时抛出：环境没装齐，工具的手柄功能无法工作。 */
export class HidUnavailable extends Error {}

/** 引入 node-hid；缺失时抛 HidUnavailable，由调用方决定怎么报（命令行退出码 2）。 */
export function loadHid() {
  try {
    // node-hid 是 CJS 原生模块，这里转成统一的枚举/开关句柄面。
    const hid = require('node-hid');
    return {
      enumerate: () => hid.devices(),
      /** 打开接口路径上的设备：返回 {read, write, close} 的异步面。 */
      open: async (path) => {
        const device = await hid.HIDAsync.open(path);
        return {
          read: () => device.read(),
          write: (data) => device.write(data),
          close: () => device.close(),
        };
      },
    };
  } catch (exc) {
    throw new HidUnavailable(`缺少 node-hid：在仓库根执行 pnpm install 后重试（${exc.message}）`);
  }
}

/** 枚举手柄用途的 HID 接口（Generic Desktop / Joystick 与 Game Pad）。 */
export function gamepadUsages(infos) {
  return infos.filter((info) => (info.usagePage ?? 0) === GAMEPAD_USAGE_PAGE
        && GAMEPAD_USAGES.includes(info.usage ?? 0));
}

/** 按设备树把候选拆成真实与虚拟两份（isVirtual 可注入便于测试）。 */
export function partitionVirtual(infos, isVirtual = isVirtualPad) {
  const real = [];
  const virtual = [];
  for (const info of infos) {
    if (isVirtual(info.path)) {
      virtual.push(info);
    } else {
      real.push(info);
    }
  }
  return [real, virtual];
}

/** 枚举候选手柄接口：同一只手柄可能有多个 HID 接口，这里只留手柄用途的；
 * ViGEm 之类的虚拟手柄排除在外（见 partitionVirtual）。 */
export function listCandidates(infos) {
  return partitionVirtual(gamepadUsages(infos))[0];
}

/** 被候选清单排除的虚拟手柄（--list 里标注展示）。 */
export function listVirtualPads(infos) {
  return partitionVirtual(gamepadUsages(infos))[1];
}

/** 连接方式：node-hid 未给 busType 时按设备树推导（USB / 蓝牙枚举器）。 */
export function connFor(info) {
  if (info.busType === 'usb') {
    return CONN_USB;
  }
  if (info.busType === 'bluetooth') {
    return CONN_BT;
  }
  return connOfHidPath(info.path);
}

/** 按 --vid / --pid 过滤候选，取第一只；图形界面用 args.padPath 钉住具体接口。 */
export function pickDevice(args, infos) {
  const candidates = listCandidates(infos);
  for (const info of candidates) {
    if (args.vid != null && info.vendorId !== args.vid) {
      continue;
    }
    if (args.pid != null && info.productId !== args.pid) {
      continue;
    }
    if (args.padPath != null && info.path !== args.padPath) {
      continue;
    }
    return info;
  }
  return null;
}

export function identityPayload(info, reportId, reportLen) {
  return deviceId(
    familyForVendor(info.vendorId),
    connFor(info),
    info.vendorId,
    info.productId,
    reportId,
    reportLen,
  );
}

export function describe(info) {
  const bus = connFor(info);
  return `${info.product ?? '?'} `
        + `${info.vendorId.toString(16).padStart(4, '0')}:${info.productId.toString(16).padStart(4, '0')} `
        + `${FAMILY_NAMES[familyForVendor(info.vendorId)] ?? '?'} `
        + `${CONN_NAMES[bus] ?? '-'} if=${info.interface ?? 0}`;
}
