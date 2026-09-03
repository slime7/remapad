/**
 * Remapad 产品控制面协议预留。
 *
 * 目标是描述 USB 输入、NS2 手柄状态、BLE 配对/广播和设备管理消息；
 * 它与 PocketJS 官方 ESP-IDF UI binding 分离，当前尚未接入实际传输层。
 */

/** 手柄工作模式。 */
export type ControllerMode = 'ble' | 'usb' | 'handheld';

/** 手柄连接与配对状态。 */
export type PairingState = 'idle' | 'scanning' | 'pairing' | 'paired' | 'connected' | 'error';

/** 手柄设备型号。 */
export type ControllerModel = 'pro-controller-2' | 'joycon-l' | 'joycon-r';

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
  | { t: 'startPairing'; id: number }
  | { t: 'stopPairing'; id: number }
  | { t: 'triggerRumble'; id: number; frequencyHz: number; amplitude: number; durationMs: number }
  | { t: 'calibrateSensors'; id: number }
  | { t: 'reboot'; id: number };

/** 产品控制面返回的应答或主动事件。 */
export type DeviceMsg =
  | { t: 'ready'; id: number; chip: string; firmwareVersion: string; psramSize: number }
  | { t: 'systemStatus'; id: number; battery: BatteryInfo; backlight: number; mode: ControllerMode; pairing: PairingState; controller: ControllerModel }
  | { t: 'backlightSet'; id: number; brightness: number; success: boolean }
  | { t: 'pairingResult'; id: number; state: PairingState; message?: string }
  | { t: 'rumbleAck'; id: number; success: boolean }
  | { t: 'error'; id: number; code: string; message: string }
  | { t: 'batteryChanged'; battery: BatteryInfo }
  | { t: 'pairingStateChanged'; state: PairingState }
  | { t: 'buttonEvent'; buttons: ControllerButtons }
  | { t: 'lowBatteryAlert'; percentage: number };
