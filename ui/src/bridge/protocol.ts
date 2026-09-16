/**
 * Remapad 产品控制面协议。
 *
 * 描述 USB 输入、NS2 手柄状态、BLE 配对/广播和设备管理消息；
 * 与 PocketJS 官方 ESP-IDF UI binding 分离，经 __nativeBridge JSON 传输。
 */

/** 手柄工作模式。 */
export type ControllerMode = 'ble' | 'usb' | 'handheld';

/** USB 控制器角色：device=插电脑（COM/烧录/日志），otg=插电脑（OTG 非串口），host=插手柄（读取输入）。 */
export type UsbRole = 'device' | 'otg' | 'host';

/**
 * 手柄连接与配对状态：idle=未配对且静默、paired=已配对且静默（等用户按连接键）、
 * scanning=配对流程在发发现广播、advertising=连接窗口在发回连形态、
 * pairing=已连接但注册握手未完成、connected=已连接并可输入。
 */
export type PairingState =
  | 'idle'
  | 'scanning'
  | 'advertising'
  | 'pairing'
  | 'paired'
  | 'connected'
  | 'error';

/** 手柄设备型号。 */
export type ControllerModel = 'pro-controller-2' | 'joycon-l' | 'joycon-r';

/** 手柄形态：Pro 手柄（默认）或 JoyCon 组合（左 + 右）。 */
export type ControllerType = 'pro' | 'joycon';

/** 手柄身份配置：类型 + 机身配色（0xRRGGBB）。颜色选择 UI 预留，字段先随配置持久化。 */
export interface ControllerConfig {
  type: ControllerType;
  bodyColor: number;
  buttonColor: number;
  gripColor: number;
}

/** 各手柄身份对外的蓝牙地址（显示序大写十六进制；host 未同步时为空串）。 */
export interface ControllerAddresses {
  pro: string;
  left: string;
  right: string;
}

/** 调试注入的按键（调试页按键指令区）：home 就是实体手柄的 HOME——注入按下后
 *  由固件按同一条语义处理（主机在线时当主页键上报，未连接时开唤醒窗口把主机
 *  叫起来并连上），ui 是手柄操控屏幕的组合键（L1+R1+L3+R3）。 */
export type DebugKey = 'a' | 'home' | 'ui';

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
  | { t: 'setScreenPower'; id: number; on: boolean }
  | { t: 'setControllerMode'; id: number; mode: ControllerMode }
  | { t: 'setUsbRole'; id: number; role: UsbRole }
  | { t: 'getControllerConfig'; id: number }
  | { t: 'setControllerConfig'; id: number; config: ControllerConfig }
  | { t: 'connect'; id: number }
  | { t: 'disconnect'; id: number }
  | { t: 'startPairing'; id: number }
  | { t: 'unpair'; id: number }
  | { t: 'pressLr'; id: number }
  | { t: 'triggerRumble'; id: number; frequencyHz: number; amplitude: number; durationMs: number }
  | { t: 'debugKey'; id: number; key: DebugKey }
  | { t: 'calibrateSensors'; id: number }
  | { t: 'powerOff'; id: number }
  | { t: 'reboot'; id: number };

/** 产品控制面返回的应答或主动事件。 */
export type DeviceMsg =
  | { t: 'ready'; id: number; chip: string; firmwareVersion: string; psramSize: number }
  | {
      t: 'systemStatus';
      id: number;
      battery: BatteryInfo;
      backlight: number;
      /** 息屏（背光关闭）状态，PWR 键或命令切换。 */
      screenOn: boolean;
      mode: ControllerMode;
      pairing: PairingState;
      controller: ControllerModel | null;
      usbRole: UsbRole;
      usbRoleActive: boolean;
      /** 主机下发的玩家序号灯掩码（Command 0x09）：bit0-3 对应四格指示灯，
       *  未连接主机时为 0。 */
      playerLed: number;
      /** 手柄操控模式：组合键把输入收给屏幕，期间不向主机输出（见 dp/dp_ui.h）。 */
      padUiMode: boolean;
      uptimeMs: number;
      /** 内部堆内存：可用 / 总量（字节）。 */
      heapFree: number;
      heapSize: number;
      /** PSRAM 可用量（字节），总量来自 ready.psramSize。 */
      psramFree: number;
    }
  | { t: 'backlightSet'; id: number; brightness: number; success: boolean }
  | { t: 'screenPowerSet'; id: number; on: boolean }
  | { t: 'screenPowerChanged'; on: boolean }
  | {
      t: 'controllerConfig';
      id: number;
      config: ControllerConfig;
      /** 身份信息卡展示的对外地址：Pro 公共伪装地址，JoyCon 左右各自派生。 */
      addresses: ControllerAddresses;
    }
  | { t: 'controllerConfigSet'; id: number; config: ControllerConfig; success: boolean }
  /* 这三条应答只回状态，不回可上屏的文案：屏幕文本一律取自 ui/src 里的
   * 字面量（构建期字体字符集按源码字面量扫描烘焙），固件回发的文本直接
   * 渲染会显示成豆腐块。 */
  | { t: 'usbRoleSet'; id: number; role: UsbRole; active: boolean }
  /** connect / disconnect / startPairing 的应答：只回状态，文案由 UI 给出。 */
  | { t: 'pairingResult'; id: number; state: PairingState }
  | { t: 'unpairResult'; id: number; state: PairingState }
  | { t: 'pressLrAck'; id: number; success: boolean }
  | { t: 'rumbleAck'; id: number; success: boolean }
  | { t: 'debugKeySet'; id: number; key: DebugKey }
  | { t: 'powerOffAck'; id: number }
  | { t: 'rebooting'; id: number }
  | { t: 'error'; id: number; code: string; message: string }
  | { t: 'batteryChanged'; battery: BatteryInfo }
  | { t: 'usbRoleChanged'; role: UsbRole; active: boolean }
  | { t: 'pairingStateChanged'; state: PairingState }
  | { t: 'playerLedChanged'; led: number }
  | { t: 'padUiModeChanged'; on: boolean }
  | { t: 'buttonEvent'; buttons: ControllerButtons }
  /** 关机被外部供电拦下（USB 供电时电源锁存被旁路，系统仍在运行）。 */
  | { t: 'powerOffBlocked' }
  | { t: 'lowBatteryAlert'; percentage: number };
