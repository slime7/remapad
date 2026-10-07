// Windows 设备树查询：hidapi 接口路径 → 实例 ID、祖先链与总线归属。
// ViGEm 之类的虚拟手柄与蓝牙/USB 归属都从这里来（node-hid 不带 busType）。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import koffi from 'koffi';

const cfgmgr32 = koffi.load('cfgmgr32.dll');
const CM_Locate_DevNodeW = cfgmgr32.func('__stdcall', 'CM_Locate_DevNodeW', 'uint32', [
  koffi.out(koffi.pointer('uint32')), 'str16', 'uint32',
]);
const CM_Get_Parent = cfgmgr32.func('__stdcall', 'CM_Get_Parent', 'uint32', [
  koffi.out(koffi.pointer('uint32')), 'uint32', 'uint32',
]);
const CM_Get_Device_IDW = cfgmgr32.func('__stdcall', 'CM_Get_Device_IDW', 'uint32', [
  'uint32', 'uint16 *', 'uint32', 'uint32',
]);

const MAX_DEVICE_ID_LEN = 200;

/** hidapi 设备接口路径 → 设备树实例 ID。
 *
 * 路径形如 \\?\HID#VID_054C&PID_0DF2&MI_03#8&2f3d4f&0&0000#{接口 GUID}，
 * 实例 ID 是前三段的 # 换 \：HID\VID_054C&PID_0DF2&MI_03\8&2f3d4f&0&0000。
 */
export function instanceIdOfHidPath(path) {
  if (typeof path !== 'string' || !path.startsWith('\\\\?\\')) {
    return null;
  }
  const parts = path.slice(4).split('#');
  // 末段是接口 GUID，缺它就不是完整的设备接口路径。
  if (parts.length < 4 || !parts.slice(0, 3).every(Boolean)) {
    return null;
  }
  return parts.slice(0, 3).join('\\');
}

function parentId(devinst) {
  const parentOut = [0];
  if (CM_Get_Parent(parentOut, devinst, 0) !== 0) {
    return null;
  }
  const buffer = Buffer.alloc(MAX_DEVICE_ID_LEN * 2);
  if (CM_Get_Device_IDW(parentOut[0], buffer, MAX_DEVICE_ID_LEN, 0) !== 0) {
    return null;
  }
  const text = buffer.toString('utf16le');
  const end = text.indexOf('\0');
  return end < 0 ? text : text.slice(0, end);
}

/** 沿设备树向上的实例 ID 链（查不到就给已拿到的部分）。 */
export function ancestorChain(instance) {
  const chain = [];
  const devinst = [0];
  if (CM_Locate_DevNodeW(devinst, instance, 0) !== 0) {
    return chain;
  }
  let current = devinst[0];
  for (let i = 0; i < 8; i++) {
    const id = parentId(current);
    if (id === null) {
      break;
    }
    chain.push(id);
    const parentOut = [0];
    if (CM_Get_Parent(parentOut, current, 0) !== 0) {
      break;
    }
    current = parentOut[0];
  }
  return chain;
}

/** 按设备树判断是不是 ViGEm 之类的虚拟手柄。
 *
 * Moonlight/Sunshine 串流时会在主机上虚拟一块 DS4（ViGEmBus 总线），hidapi
 * 把它枚举成普通 USB 手柄，按顺序选柄会把它当桥接目标抓走——输入转发与震动
 * 写回全进虚拟设备，真手柄反而时有时无。hidapi 的路径里看不出虚拟与否，这里
 * 沿设备树向上查祖先的设备 ID，任一代以 VIGEM 开头即虚拟；查不到实例或祖先
 * （非常规设备树）一律按真实手柄处理，绝不静默丢设备。
 */
export function isVirtualPad(path) {
  const instance = instanceIdOfHidPath(path);
  if (instance === null) {
    return false;
  }
  return ancestorChain(instance).some((id) => id.toUpperCase().startsWith('VIGEM'));
}

/** 手柄接口的连接方式（1 = USB，2 = 蓝牙，0 = 未知）：看祖先链的总线枚举器。
 * node-hid 不暴露 busType，USB 祖先（USB\）与蓝牙祖先（BTHENUM\、BTHLE\）都在设备树里。 */
export function connOfHidPath(path) {
  const instance = instanceIdOfHidPath(path);
  if (instance === null) {
    return 0;
  }
  for (const id of ancestorChain(instance)) {
    const upper = id.toUpperCase();
    if (upper.startsWith('USB\\')) {
      return 1;
    }
    if (upper.startsWith('BTHENUM\\') || upper.startsWith('BTHLE\\') || upper.startsWith('BTH\\')) {
      return 2;
    }
  }
  return 0;
}
