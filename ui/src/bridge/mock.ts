import type { BatteryInfo, ControllerMode, DeviceCmd, DeviceMsg, PairingState, UsbRole } from './protocol';

interface MockHardwareState {
  battery: BatteryInfo;
  backlight: number;
  mode: ControllerMode;
  pairing: PairingState;
  controller: 'pro-controller-2' | 'joycon-l' | 'joycon-r' | null;
  usbRole: UsbRole;
  /** host 数据面未接入，mock 里只有 device 角色是"生效"的。 */
  usbRoleActive: boolean;
  bootAt: number;
  heapSize: number;
  heapFree: number;
  psramFree: number;
}

const state: MockHardwareState = {
  battery: {
    voltageMv: 4120,
    percentage: 88,
    charging: false,
  },
  backlight: 40,
  mode: 'ble',
  pairing: 'idle',
  controller: null,
  usbRole: 'device',
  usbRoleActive: true,
  bootAt: Date.now(),
  heapSize: 320 * 1024,
  heapFree: 186 * 1024,
  psramFree: Math.round(2.8 * 1024 * 1024),
};

let pairingTimers: ReturnType<typeof setTimeout>[] = [];

function clearPairingTimers(): void {
  pairingTimers.forEach(clearTimeout);
  pairingTimers = [];
}

function broadcast(reply: (msg: DeviceMsg) => void, msg: DeviceMsg): void {
  pairingTimers.push(setTimeout(() => reply(msg), 0));
}

function setPairing(
  reply: (msg: DeviceMsg) => void,
  pairing: PairingState,
): void {
  state.pairing = pairing;
  broadcast(reply, { t: 'pairingStateChanged', state: pairing });
}

/** 浏览器环境下的产品控制面协议 mock；不模拟 PocketJS UI binding。 */
export function mockHandleCmd(cmd: DeviceCmd, reply: (msg: DeviceMsg) => void): void {
  const id = cmd.id;

  switch (cmd.t) {
    case 'hello':
      reply({
        t: 'ready',
        id,
        chip: 'ESP32-S3 (Mock)',
        firmwareVersion: 'v0.2.0-sim',
        psramSize: 8 * 1024 * 1024,
      });
      break;

    case 'getSystemStatus':
      reply({
        t: 'systemStatus',
        id,
        battery: { ...state.battery },
        backlight: state.backlight,
        mode: state.mode,
        pairing: state.pairing,
        controller: state.controller,
        usbRole: state.usbRole,
        usbRoleActive: state.usbRoleActive,
        uptimeMs: Date.now() - state.bootAt,
        heapFree: state.heapFree,
        heapSize: state.heapSize,
        psramFree: state.psramFree,
      });
      break;

    case 'setBacklight':
      state.backlight = Math.max(0, Math.min(100, cmd.brightness));
      reply({
        t: 'backlightSet',
        id,
        brightness: state.backlight,
        success: true,
      });
      break;

    case 'setControllerMode':
      state.mode = cmd.mode;
      reply({
        t: 'error',
        id,
        code: 'NOT_IMPLEMENTED',
        message: `Controller mode ${cmd.mode} needs the data plane`,
      });
      break;

    case 'setUsbRole': {
      state.usbRole = cmd.role;
      state.usbRoleActive = cmd.role !== 'host';
      const message =
        cmd.role === 'host' ? 'USB host 数据面未接入，切换暂不生效' : undefined;
      reply({ t: 'usbRoleSet', id, role: cmd.role, active: state.usbRoleActive, message });
      broadcast(reply, { t: 'usbRoleChanged', role: cmd.role, active: state.usbRoleActive });
      break;
    }

    case 'startPairing':
      if (state.pairing === 'scanning' || state.pairing === 'pairing') {
        reply({ t: 'pairingResult', id, state: state.pairing, message: '配对已在进行中' });
        break;
      }
      clearPairingTimers();
      setPairing(reply, 'scanning');
      reply({ t: 'pairingResult', id, state: 'scanning', message: '开始广播（模拟）' });
      pairingTimers.push(setTimeout(() => setPairing(reply, 'pairing'), 1500));
      pairingTimers.push(setTimeout(() => setPairing(reply, 'paired'), 4200));
      break;

    case 'stopPairing':
      clearPairingTimers();
      // 停止搜索只退出配对模式：未配成则回 idle，已配对则凭证保持。
      if (state.pairing === 'scanning' || state.pairing === 'pairing') {
        setPairing(reply, 'idle');
      }
      reply({ t: 'pairingResult', id, state: state.pairing, message: '已退出配对模式' });
      break;

    case 'unpair':
      clearPairingTimers();
      state.controller = null;
      setPairing(reply, 'idle');
      reply({ t: 'unpairResult', id, state: 'idle', message: '已解除配对' });
      break;

    case 'triggerRumble':
      reply({ t: 'rumbleAck', id, success: false });
      break;

    case 'debugKey':
      // 浏览器预览没有数据面，只回执确认供调试页高亮反馈。
      reply({ t: 'debugKeySet', id, key: cmd.key });
      break;

    case 'calibrateSensors':
      reply({
        t: 'error',
        id,
        code: 'NOT_IMPLEMENTED',
        message: 'Sensors are not wired yet',
      });
      break;

    case 'reboot':
      reply({ t: 'rebooting', id });
      break;

    default:
      reply({
        t: 'error',
        id,
        code: 'UNSUPPORTED_CMD',
        message: 'Mock does not support this command',
      });
      break;
  }
}
