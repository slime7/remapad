/**
 * Remapad 硬件交互协议契约定义 (借鉴自 pocket-youtube 的 protocol 架构)
 *
 * 定义 UI 视图层与底层微控制器 (ESP32-S3) / 伴随宿主之间的双向消息格式。
 * 所有请求均携带自增 id，硬件回复时回显该 id，实现无序异步匹配；
 * 硬件主动上报的异步事件 (如按键中断、电量通知) 不携带 id。
 */

/** 手柄工作模式 */
export type ControllerMode = 'ble' | 'usb' | 'handheld';

/** 手柄连接与配对状态 */
export type PairingState = 'idle' | 'scanning' | 'pairing' | 'paired' | 'connected' | 'error';

/** 手柄设备型号 */
export type ControllerModel = 'pro-controller-2' | 'joycon-l' | 'joycon-r';

/** 电池状态数据包 */
export interface BatteryInfo {
  voltageMv: number;
  percentage: number;
  charging: boolean;
}

/** 手柄按键状态掩码 */
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

/** UI 前端发送给硬件的指令 (DeviceCmd) */
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

/** 硬件返回的应答或主动推送的事件 (DeviceMsg) */
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
