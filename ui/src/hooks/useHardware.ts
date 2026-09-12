/**
 * 产品控制面（bridge）在 UI 侧的状态聚合。
 *
 * App 壳在 setup 中调用一次 useHardware() 完成握手、轮询与事件订阅；页面
 * 组件直接导入 hw 状态与动作，避免 prop 层层透传。onFrame 驱动轮询与本地
 * uptime 推算，兼容设备（无定时器）与浏览器预览两种环境。
 */
import { reactive } from 'vue';
import { onFrame } from '@pocketjs/framework/vue-vapor/lifecycle';
import { hardware } from '../bridge/driver';
import type { BatteryInfo, DebugKey, DeviceMsg, PairingState, UsbRole } from '../bridge/protocol';

export interface HardwareUiState {
  /** bridge 握手成功（原生固件或浏览器 mock）。 */
  linkReady: boolean;
  chip: string;
  firmwareVersion: string;
  psramSize: number;
  /** 内部堆内存：可用 / 总量（字节）。 */
  heapFree: number;
  heapSize: number;
  /** PSRAM 可用量（字节）。 */
  psramFree: number;
  battery: BatteryInfo;
  backlight: number;
  pairing: PairingState;
  pairingMessage: string;
  /** USB 手柄（数据面未接入，恒为 null）。 */
  controller: string | null;
  usbRole: UsbRole;
  /** USB host 数据面未接入，host 角色仅记录请求。 */
  usbRoleActive: boolean;
  /** 本地推算的实时开机时长。 */
  uptimeMs: number;
  /** 已发送重启命令。 */
  rebooting: boolean;
}

export const hw = reactive<HardwareUiState>({
  linkReady: false,
  chip: '',
  firmwareVersion: '',
  psramSize: 0,
  heapFree: 0,
  heapSize: 0,
  psramFree: 0,
  battery: { voltageMv: 0, percentage: 0, charging: false },
  backlight: 40,
  pairing: 'idle',
  pairingMessage: '',
  controller: null,
  usbRole: 'device',
  usbRoleActive: true,
  uptimeMs: 0,
  rebooting: false,
});

const POLL_TICKS = 300; // 60Hz × 5s
const tickHz: number = (globalThis as unknown as { ui?: { __tickHz?: number } }).ui?.__tickHz ?? 60;

let started = false;
let ticks = 0;
let uptimeSyncTicks = 0;

function applySystemStatus(msg: Extract<DeviceMsg, { t: 'systemStatus' }>): void {
  hw.battery = msg.battery;
  hw.backlight = msg.backlight;
  hw.pairing = msg.pairing;
  hw.controller = msg.controller;
  hw.usbRole = msg.usbRole;
  hw.usbRoleActive = msg.usbRoleActive;
  hw.uptimeMs = msg.uptimeMs;
  hw.heapFree = msg.heapFree;
  hw.heapSize = msg.heapSize;
  hw.psramFree = msg.psramFree;
  uptimeSyncTicks = ticks;
}

function refreshStatus(): void {
  hardware.send({ t: 'getSystemStatus' }, (msg) => {
    if (msg.t === 'systemStatus') {
      applySystemStatus(msg);
    }
  });
}

export function startPairing(): void {
  hardware.send({ t: 'startPairing' }, (msg) => {
    if (msg.t === 'pairingResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = msg.message ?? '';
    } else if (msg.t === 'error') {
      hw.pairingMessage = msg.message;
    }
  });
}

export function stopPairing(): void {
  hardware.send({ t: 'stopPairing' }, (msg) => {
    if (msg.t === 'pairingResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = msg.message ?? '';
    }
  });
}

export function unpair(): void {
  hardware.send({ t: 'unpair' }, (msg) => {
    if (msg.t === 'unpairResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = msg.message ?? '';
    }
  });
}

export function setUsbRole(role: UsbRole): void {
  hardware.send({ t: 'setUsbRole', role }, (msg) => {
    if (msg.t === 'usbRoleSet') {
      hw.usbRole = msg.role;
      hw.usbRoleActive = msg.active;
      hw.pairingMessage = msg.message ?? '';
    }
  });
}

export function setBacklight(brightness: number): void {
  const clamped = Math.max(0, Math.min(100, Math.round(brightness)));
  hw.backlight = clamped;
  hardware.send({ t: 'setBacklight', brightness: clamped }, (msg) => {
    if (msg.t === 'backlightSet' && msg.success) {
      hw.backlight = msg.brightness;
    }
  });
}

/** 调试页按键注入：onAck 在固件确认写入数据面后触发，用于按钮高亮反馈。 */
export function sendDebugKey(key: DebugKey, onAck?: () => void): void {
  hardware.send({ t: 'debugKey', key }, (msg) => {
    if (msg.t === 'debugKeySet' && onAck) {
      onAck();
    }
  });
}

export function rebootDevice(): void {
  hw.rebooting = true;
  hardware.send({ t: 'reboot' }, (msg) => {
    if (msg.t === 'rebooting') {
      hw.pairingMessage = '重启中…';
    }
  });
}

/** App 壳 setup 时调用一次；页面组件直接读 hw / 动作函数。 */
export function useHardware(): void {
  if (started) {
    return;
  }
  started = true;


  hardware.send({ t: 'hello', clientVersion: 'remapad-ui/0.2.0' }, (msg) => {
    if (msg.t === 'ready') {
      hw.linkReady = true;
      hw.chip = msg.chip;
      hw.firmwareVersion = msg.firmwareVersion;
      hw.psramSize = msg.psramSize;
    }
  });
  refreshStatus();

  hardware.onEvent((msg) => {
    switch (msg.t) {
      case 'pairingStateChanged':
        hw.pairing = msg.state;
        // 阶段提示只在命令应答里给一次，状态流转后清掉避免残留。
        hw.pairingMessage = '';
        break;
      case 'usbRoleChanged':
        hw.usbRole = msg.role;
        hw.usbRoleActive = msg.active;
        break;
      case 'batteryChanged':
        hw.battery = msg.battery;
        break;
      default:
        break;
    }
  });

  onFrame(() => {
    ticks++;
    if (ticks % POLL_TICKS === 0) {
      refreshStatus();
    }
    if (ticks % tickHz === 0) {
      // 两次状态轮询之间按帧数本地推算 uptime，避免每帧改响应式状态。
      hw.uptimeMs += (ticks - uptimeSyncTicks) * (1000 / tickHz);
      uptimeSyncTicks = ticks;
    }
  });
}
