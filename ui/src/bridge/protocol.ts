/**
 * Remapad 产品控制面协议预留。
 *
 * 目标是描述 USB 输入、NS2 手柄状态、BLE 配对/广播和设备管理消息；
 * 它与 PocketJS 官方 ESP-IDF UI binding 分离，当前尚未接入实际传输层。
 */

/** 手柄工作模式。 */
export type ControllerMode = 'ble' | 'usb' | 'handheld';

/** USB 控制器角色：device=插电脑（COM/烧录/日志），otg=插电脑（OTG 非串口），host=插手柄（读取输入）。 */
export type UsbRole = 'device' | 'otg' | 'host';

/** 手柄连接与配对状态。 */
export type PairingState = 'idle' | 'scanning' | 'pairing' | 'paired' | 'connected' | 'error';

/** 手柄设备型号。 */
export type ControllerModel = 'pro-controller-2' | 'joycon-l' | 'joycon-r';

/** 调试注入的按键（调试页按键指令区）；lr 表示同时按下 L 和 R。 */
export type DebugKey = 'a' | 'home' | 'lr';

/** 电池状态数据包。 */
export interface BatteryInfo {
  voltageMv: number;
  percentage: number;
  charging: boolean;
}

/** 手柄按键状态掩码。 */
export interface ControllerButtons {
  a?: boolean;
  b?: boolean;
  x?: boolean;
  y?: boolean;
  dpadUp?: boolean;
  dpadDown?: boolean;
  dpadLeft?: boolean;
  dpadRight?: boolean;
  l?: boolean;
  r?: boolean;
  zl?: boolean;
  zr?: boolean;
  plus?: boolean;
  minus?: boolean;
  home?: boolean;
  capture?: boolean;
}

/** 产品控制面发送给固件的命令。 */
export type DeviceCmd =
  | { t: 'hello'; id: number; clientVersion: string }
  | { t: 'getSystemStatus'; id: number }
  | { t: 'setBacklight'; id: number; brightness: number }
  | { t: 'setControllerMode'; id: number; mode: ControllerMode }
  | { t: 'setUsbRole'; id: number; role: UsbRole }
  | { t: 'startPairing'; id: number }
  | { t: 'stopPairing'; id: number }
  | { t: 'triggerRumble'; id: number; frequencyHz: number; amplitude: number; durationMs: number }
  | { t: 'debugKey'; id: number; key: DebugKey }
  | { t: 'calibrateSensors'; id: number }
  | { t: 'reboot'; id: number };

/** 产品控制面返回的应答或主动事件。 */
export type DeviceMsg =
  | { t: 'ready'; id: number; chip: string; firmwareVersion: string; psramSize: number }
  | {
      t: 'systemStatus';
      id: number;
      battery: BatteryInfo;
      backlight: number;
      mode: ControllerMode;
      pairing: PairingState;
      controller: ControllerModel | null;
      usbRole: UsbRole;
      usbRoleActive: boolean;
      uptimeMs: number;
      /** 内部堆内存：可用 / 总量（字节）。 */
      heapFree: number;
      heapSize: number;
      /** PSRAM 可用量（字节），总量来自 ready.psramSize。 */
      psramFree: number;
    }
  | { t: 'backlightSet'; id: number; brightness: number; success: boolean }
  | { t: 'usbRoleSet'; id: number; role: UsbRole; active: boolean; message?: string }
  | { t: 'pairingResult'; id: number; state: PairingState; message?: string }
  | { t: 'rumbleAck'; id: number; success: boolean }
  | { t: 'debugKeySet'; id: number; key: DebugKey }
  | { t: 'rebooting'; id: number }
  | { t: 'error'; id: number; code: string; message: string }
  | { t: 'batteryChanged'; battery: BatteryInfo }
  | { t: 'usbRoleChanged'; role: UsbRole; active: boolean }
  | { t: 'pairingStateChanged'; state: PairingState }
  | { t: 'buttonEvent'; buttons: ControllerButtons }
  | { t: 'lowBatteryAlert'; percentage: number };
