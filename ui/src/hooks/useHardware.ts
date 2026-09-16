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
import type {
  BatteryInfo,
  ControllerAddresses,
  ControllerConfig,
  DebugKey,
  DeviceMsg,
  PairingState,
  UsbRole,
} from '../bridge/protocol';
import type { PairingNotice, RoleNotice } from '../utils';

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
  /** 息屏状态（背光关闭），PWR 键或命令切换。 */
  screenOn: boolean;
  pairing: PairingState;
  pairingMessage: PairingNotice;
  /** 手柄身份配置（类型 + 配色），持久化在固件 NVS。 */
  controllerConfig: ControllerConfig;
  /** 各身份对外的蓝牙地址（显示序；host 未同步时为空串）。 */
  controllerAddresses: ControllerAddresses;
  controller: string | null;
  usbRole: UsbRole;
  /** USB host 数据面未接入，host 角色仅记录请求。 */
  usbRoleActive: boolean;
  /** 主机下发的玩家序号灯掩码（bit0-3 对应首页四格指示灯），无主机时为 0。 */
  playerLed: number;
  /** 手柄操控模式：组合键把输入收给屏幕，期间屏幕由手柄按键操作。 */
  padUiMode: boolean;
  /** 模式页角色切换的一次性提示（如桥接禁切原因）。 */
  roleMessage: RoleNotice;
  /** 本地推算的实时开机时长。 */
  uptimeMs: number;
  /** 实测帧率（帧/秒）：只在系统页可见时采样，null = 尚无样本。 */
  fps: number | null;
  /** 已发送重启命令。 */
  rebooting: boolean;
  /** 已确认关机、等待断电（USB 供电时断不了，固件回报后遮罩收起）。 */
  poweringOff: boolean;
}

/** 手柄配置默认值：Pro + 深灰配色（与固件出厂块占位一致）。 */
const DEFAULT_CONTROLLER_CONFIG: ControllerConfig = {
  type: 'pro',
  bodyColor: 0x232323,
  buttonColor: 0x3c3c3c,
  gripColor: 0x2e2e2e,
};

/** 地址就绪前的占位值（固件在 host 同步前给空串）。 */
const DEFAULT_CONTROLLER_ADDRESSES: ControllerAddresses = {
  pro: '',
  left: '',
  right: '',
};

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
  screenOn: true,
  pairing: 'idle',
  pairingMessage: '',
  controllerConfig: { ...DEFAULT_CONTROLLER_CONFIG },
  controllerAddresses: { ...DEFAULT_CONTROLLER_ADDRESSES },
  controller: null,
  usbRole: 'device',
  usbRoleActive: true,
  playerLed: 0,
  padUiMode: false,
  roleMessage: '',
  uptimeMs: 0,
  fps: null,
  rebooting: false,
  poweringOff: false,
});

const POLL_TICKS = 300; // 60Hz × 5s
const tickHz: number = (globalThis as unknown as { ui?: { __tickHz?: number } }).ui?.__tickHz ?? 60;
/** 帧率采样窗口：一秒的虚拟帧。健康时即一秒墙钟，掉帧时窗口相应拉长。 */
const FPS_WINDOW_TICKS = tickHz;

let started = false;
let ticks = 0;
let uptimeSyncTicks = 0;
/** 系统页是否在画面上：每秒 uptime 推算只服务这一页，页面不在就停算
 *  （隐藏页的响应式绑定仍挂着，喂进去的每次变更都会变成原生文本重写与
 *  布局重排；见 SystemPage 的易变行门控）。 */
let systemInfoLive = false;
/** 采样开关（系统页可见时置位）、上次请求的帧号与当前窗口的锚点。 */
let samplingFps = false;
let fpsRequestFrame = -FPS_WINDOW_TICKS;
let fpsAnchorFrame = -1;
let fpsAnchorUptimeMs = 0;

function applySystemStatus(msg: Extract<DeviceMsg, { t: 'systemStatus' }>): void {
  /* battery 按字段原地写：整个对象换新会让所有读它的绑定（状态栏电池、
   * 系统页电池行）每 5 秒重跑一次，即使数值没变——同值字段写不触发。 */
  hw.battery.voltageMv = msg.battery.voltageMv;
  hw.battery.percentage = msg.battery.percentage;
  hw.battery.charging = msg.battery.charging;
  hw.backlight = msg.backlight;
  hw.screenOn = msg.screenOn;
  hw.pairing = msg.pairing;
  hw.controller = msg.controller;
  hw.usbRole = msg.usbRole;
  hw.usbRoleActive = msg.usbRoleActive;
  hw.playerLed = msg.playerLed;
  hw.padUiMode = msg.padUiMode;
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

/**
 * 采一次实时帧率：向设备要一份状态，用应答里的设备时钟和本地帧计数算
 * 上一个窗口的实测帧率。进页后的第一个窗口只建立锚点，所以数字要等
 * 约一秒；设备不回或命令失败时锚点不动，下一个窗口继续请求，不会卡住。
 */
function requestFrameRateSample(): void {
  const frame = ticks;
  fpsRequestFrame = frame;
  hardware.send({ t: 'getSystemStatus' }, (msg) => {
    if (msg.t !== 'systemStatus' || !samplingFps) {
      return;
    }
    applySystemStatus(msg);
    const elapsedMs = msg.uptimeMs - fpsAnchorUptimeMs;
    if (fpsAnchorFrame >= 0 && elapsedMs > 0) {
      hw.fps = ((frame - fpsAnchorFrame) * 1000) / elapsedMs;
    }
    fpsAnchorFrame = frame;
    fpsAnchorUptimeMs = msg.uptimeMs;
  });
}

/**
 * 系统页可见时才采样实时帧率：进页立刻取一次设备时钟锚点并清掉旧读数，
 * 离页停止采样（在飞的应答回来后不再写状态）。采样期间不再发常规的
 * status 轮询——采样请求本身就把同一份状态带回来了，命令频率不翻倍。
 */
export function setFrameRateSampling(on: boolean): void {
  if (on === samplingFps) {
    return;
  }
  samplingFps = on;
  hw.fps = null;
  fpsAnchorFrame = -1;
  fpsAnchorUptimeMs = 0;
  if (on) {
    requestFrameRateSample();
  }
}

/**
 * 系统页可见性同步给每秒 uptime 推算：页面不在就不再写 hw.uptimeMs，
 * 隐藏页不再被这一秒一次的文本变更喂进原生重排。恢复时补上隐藏期间的
 * 差值，开机时长保持墙钟语义。
 */
export function setSystemInfoLive(on: boolean): void {
  systemInfoLive = on;
}

/* 三个动作的提示文案都在本文件里给出：屏幕文本必须是 ui/src 的字面量，
 * 固件回发的文本不进界面（见 utils.ts 的 PairingNotice）。 */
export function startPairing(): void {
  hardware.send({ t: 'startPairing' }, (msg) => {
    if (msg.t === 'pairingResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = '广播中，等待主机连接';
    } else if (msg.t === 'error') {
      hw.pairingMessage = '配对命令未生效';
    }
  });
}

export function stopPairing(): void {
  hardware.send({ t: 'stopPairing' }, (msg) => {
    if (msg.t === 'pairingResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = '已退出配对流程';
    }
  });
}

export function unpair(): void {
  hardware.send({ t: 'unpair' }, (msg) => {
    if (msg.t === 'unpairResult') {
      hw.pairing = msg.state;
      hw.pairingMessage = '已解除配对';
    }
  });
}

/**
 * 配对页「按下 LR」：Pro 手柄向主机注入 L+R 按键（部分界面用它确认注册）；
 * JoyCon 组合触发固件的左右双机配对（两台身份同时广播/确认）。
 */
export function pressLr(): void {
  hardware.send({ t: 'pressLr' }, (msg) => {
    if (msg.t === 'pressLrAck' && !msg.success) {
      hw.pairingMessage = '当前状态无法执行 LR 配对';
    }
  });
}

export function setUsbRole(role: UsbRole): void {
  hardware.send({ t: 'setUsbRole', role }, (msg) => {
    if (msg.t === 'usbRoleSet') {
      hw.usbRole = msg.role;
      hw.usbRoleActive = msg.active;
      /* host 角色在固件侧还没接数据面：如实提示，文案由 UI 给出。 */
      hw.roleMessage =
        msg.role === 'host' && !msg.active ? 'USB host 数据面未接入，切换暂不生效' : '';
    } else if (msg.t === 'error') {
      hw.roleMessage = 'USB 角色切换未生效';
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

/** 息屏 / 亮屏（PWR 键之外的软件入口，当前无 UI 入口，保留给后续使用）。 */
export function setScreenPower(on: boolean): void {
  hardware.send({ t: 'setScreenPower', on }, (msg) => {
    if (msg.t === 'screenPowerSet') {
      hw.screenOn = msg.on;
    }
  });
}

export function setControllerConfig(config: ControllerConfig): void {
  hw.controllerConfig = { ...config };
  hardware.send({ t: 'setControllerConfig', config }, (msg) => {
    if (msg.t === 'controllerConfigSet' && msg.success) {
      hw.controllerConfig = msg.config;
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

/**
 * 关机：固件把 SYS_EN 拉低释放电源锁存。电池供电时系统立刻断电，界面停在
 * 关机中；USB 供电下锁存被旁路，固件确认自己仍存活后会回报 powerOffBlocked，
 * 界面据此收起遮罩、回到原页面。
 */
export function powerOffDevice(): void {
  hw.poweringOff = true;
  hardware.send({ t: 'powerOff' });
}

/** App 壳 setup 时调用一次；页面组件直接读 hw / 动作函数。 */
export function useHardware(): void {
  if (started) {
    return;
  }
  started = true;


  hardware.send({ t: 'hello', clientVersion: 'remapad-ui/0.4.0' }, (msg) => {
    if (msg.t === 'ready') {
      hw.linkReady = true;
      hw.chip = msg.chip;
      hw.firmwareVersion = msg.firmwareVersion;
      hw.psramSize = msg.psramSize;
    }
  });
  refreshStatus();
  hardware.send({ t: 'getControllerConfig' }, (msg) => {
    if (msg.t === 'controllerConfig') {
      hw.controllerConfig = msg.config;
      hw.controllerAddresses = msg.addresses;
    }
  });

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
      case 'screenPowerChanged':
        hw.screenOn = msg.on;
        break;
      case 'playerLedChanged':
        hw.playerLed = msg.led;
        break;
      case 'padUiModeChanged':
        hw.padUiMode = msg.on;
        break;
      case 'batteryChanged':
        hw.battery = msg.battery;
        break;
      case 'powerOffBlocked':
        hw.poweringOff = false;
        break;
      default:
        break;
    }
  });

  onFrame(() => {
    ticks++;
    if (ticks % POLL_TICKS === 0 && !samplingFps) {
      refreshStatus();
    }
    if (samplingFps && ticks - fpsRequestFrame >= FPS_WINDOW_TICKS) {
      requestFrameRateSample();
    }
    if (ticks % tickHz === 0 && systemInfoLive) {
      // 两次状态轮询之间按帧数本地推算 uptime，避免每帧改响应式状态。
      hw.uptimeMs += (ticks - uptimeSyncTicks) * (1000 / tickHz);
      uptimeSyncTicks = ticks;
    }
  });
}
