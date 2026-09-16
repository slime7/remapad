import type {
  BatteryInfo,
  ControllerAddresses,
  ControllerConfig,
  ControllerMode,
  DeviceCmd,
  DeviceMsg,
  PairingState,
  UsbRole,
} from './protocol';

interface MockHardwareState {
  battery: BatteryInfo;
  backlight: number;
  screenOn: boolean;
  mode: ControllerMode;
  pairing: PairingState;
  controller: 'pro-controller-2' | 'joycon-l' | 'joycon-r' | null;
  controllerConfig: ControllerConfig;
  usbRole: UsbRole;
  /** host 数据面未接入，mock 里只有 device 角色是"生效"的。 */
  usbRoleActive: boolean;
  /** 主机下发的玩家序号灯掩码（bit0-3），无主机时为 0。 */
  playerLed: number;
  bootAt: number;
  heapSize: number;
  heapFree: number;
  psramFree: number;
}

/** 浏览器 mock 的默认手柄配置：Pro + 占位配色（与固件出厂块一致）。 */
const DEFAULT_CONTROLLER_CONFIG: ControllerConfig = {
  type: 'pro',
  bodyColor: 0x232323,
  buttonColor: 0x3c3c3c,
  gripColor: 0x2e2e2e,
};

/** 浏览器 mock 的对外地址：与固件的派生规则同形（公共伪装地址 + 左右扩散派生）。 */
const MOCK_CONTROLLER_ADDRESSES: ControllerAddresses = {
  pro: '78:81:8C:1A:2B:3C',
  left: 'E9:D4:62:0F:14:48',
  right: 'CA:8A:D9:29:23:6F',
};

const state: MockHardwareState = {
  battery: {
    voltageMv: 4120,
    percentage: 88,
    charging: false,
  },
  backlight: 40,
  screenOn: true,
  mode: 'ble',
  pairing: 'idle',
  controller: null,
  controllerConfig: { ...DEFAULT_CONTROLLER_CONFIG },
  usbRole: 'device',
  usbRoleActive: true,
  playerLed: 0,
  bootAt: Date.now(),
  heapSize: 320 * 1024,
  heapFree: 186 * 1024,
  psramFree: Math.round(2.8 * 1024 * 1024),
};

/** 模拟主机侧的配对耗时：设备发发现广播等主机搜索、配对、握手完成。 */
const HOST_PAIR_MS = 8000;

/** 已配对身份的回连耗时：主机醒着时认回连形态直接握手，比首次配对快。 */
const HOST_RECONNECT_MS = 2000;

/** 模拟的主机侧凭证：Pro 与 JoyCon 组合在主机眼里是两台设备，各记一份。 */
const bonded: Record<'pro' | 'joycon', boolean> = { pro: false, joycon: false };

/** 当前手柄配置对应的凭证槽。 */
function bondKey(): 'pro' | 'joycon' {
  return state.controllerConfig.type === 'joycon' ? 'joycon' : 'pro';
}

/**
 * 事件出口：固件侧的状态变化是主动推送，浏览器里由 driver 在每次 send 时
 * 挂上路由回调，mock 的定时器随后用它推事件（事件没有 id，走事件监听路径）。
 */
let sink: ((msg: DeviceMsg) => void) | null = null;

export function mockAttachSink(reply: (msg: DeviceMsg) => void): void {
  sink = reply;
}

let mockTimers: ReturnType<typeof setTimeout>[] = [];

function clearMockTimers(): void {
  mockTimers.forEach(clearTimeout);
  mockTimers = [];
}

function emit(msg: DeviceMsg): void {
  sink?.(msg);
}

/** 延时动作，随下一次流程启动整体作废。 */
function later(delayMs: number, action: () => void): void {
  mockTimers.push(setTimeout(action, delayMs));
}

function setPairing(pairing: PairingState): void {
  state.pairing = pairing;
  emit({ t: 'pairingStateChanged', state: pairing });
}

/** 主机注册后下发玩家序号灯（Command 0x09）：mock 里随连接完成给出 Player 1。 */
function setPlayerLed(led: number): void {
  if (state.playerLed === led) {
    return;
  }
  state.playerLed = led;
  emit({ t: 'playerLedChanged', led });
}

/** 主机完成注册：序号灯亮起、进入已连接，当前身份的凭证落一份。 */
function hostConnects(): void {
  bonded[bondKey()] = true;
  state.controller = state.controllerConfig.type === 'joycon' ? 'joycon-l' : 'pro-controller-2';
  setPlayerLed(0b0001);
  setPairing('connected');
}

/** 配对流程（发现广播）：主机搜到并配对，配完固件自动退出流程。 */
function startPairingFlow(): void {
  setPairing('scanning');
  later(Math.round(HOST_PAIR_MS * 0.6), () => setPairing('pairing'));
  later(HOST_PAIR_MS, hostConnects);
}

/** 连接键（已配对身份）：连接窗口内发回连形态，主机看到就连上来。 */
function startConnectFlow(): void {
  setPairing('advertising');
  later(HOST_RECONNECT_MS, hostConnects);
}

/**
 * 切换手柄身份（类型或配色）：等价于旧手柄断电、新手柄上电——新身份不自动
 * 发信号（真机换上的手柄不按键也不广播），用户按连接键才连。
 */
function beginIdentityFlow(): void {
  clearMockTimers();
  state.controller = null;
  setPlayerLed(0);
  setPairing(bonded[bondKey()] ? 'paired' : 'idle');
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
        firmwareVersion: 'v0.4.0-sim',
        psramSize: 8 * 1024 * 1024,
      });
      break;

    case 'getSystemStatus':
      reply({
        t: 'systemStatus',
        id,
        battery: { ...state.battery },
        backlight: state.backlight,
        screenOn: state.screenOn,
        mode: state.mode,
        pairing: state.pairing,
        controller: state.controller,
        usbRole: state.usbRole,
        usbRoleActive: state.usbRoleActive,
        playerLed: state.playerLed,
        /* 浏览器预览没有数据面：手柄操控模式恒关，键盘随时可用。 */
        padUiMode: false,
        uptimeMs: Date.now() - state.bootAt,
        heapFree: state.heapFree,
        heapSize: state.heapSize,
        psramFree: state.psramFree,
      });
      break;

    case 'setBacklight':
      state.backlight = Math.max(0, Math.min(100, cmd.brightness));
      if (state.backlight > 0) {
        state.screenOn = true;
      }
      reply({
        t: 'backlightSet',
        id,
        brightness: state.backlight,
        success: true,
      });
      break;

    case 'setScreenPower': {
      state.screenOn = cmd.on;
      // 息屏即背光归零；亮屏恢复到最近一次的非零亮度。
      if (cmd.on) {
        state.backlight = Math.max(20, state.backlight);
      } else {
        state.backlight = 0;
      }
      reply({ t: 'screenPowerSet', id, on: state.screenOn });
      emit({ t: 'screenPowerChanged', on: state.screenOn });
      break;
    }

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
      // 桥接（otg）开发期临时禁用防误操作：后端静默跳过，不应用也不报错。
      if (cmd.role === 'otg') {
        reply({ t: 'usbRoleSet', id, role: state.usbRole, active: state.usbRoleActive });
        break;
      }
      state.usbRole = cmd.role;
      state.usbRoleActive = cmd.role !== 'host';
      reply({ t: 'usbRoleSet', id, role: cmd.role, active: state.usbRoleActive });
      emit({ t: 'usbRoleChanged', role: cmd.role, active: state.usbRoleActive });
      break;
    }

    case 'getControllerConfig':
      reply({
        t: 'controllerConfig',
        id,
        config: { ...state.controllerConfig },
        addresses: { ...MOCK_CONTROLLER_ADDRESSES },
      });
      break;

    case 'setControllerConfig': {
      /* 切换等价于「旧手柄断电、新手柄上电」：身份或配色变了就重走连接流程。 */
      const configChanged =
        state.controllerConfig.type !== cmd.config.type ||
        state.controllerConfig.bodyColor !== cmd.config.bodyColor ||
        state.controllerConfig.buttonColor !== cmd.config.buttonColor ||
        state.controllerConfig.gripColor !== cmd.config.gripColor;
      state.controllerConfig = { ...cmd.config };
      reply({
        t: 'controllerConfigSet',
        id,
        config: { ...state.controllerConfig },
        success: true,
      });
      if (configChanged) {
        beginIdentityFlow();
      }
      break;
    }

    case 'connect':
      /* 连接键：已配对发回连形态等主机连回来，未配对进配对流程发发现广播。 */
      clearMockTimers();
      if (bonded[bondKey()]) {
        startConnectFlow();
      } else {
        startPairingFlow();
      }
      reply({ t: 'pairingResult', id, state: state.pairing });
      break;

    case 'disconnect':
      /* 停止广播：收掉连接窗口与配对流程，有链路时一并断开，回到静默。 */
      clearMockTimers();
      state.controller = null;
      setPlayerLed(0);
      setPairing(bonded[bondKey()] ? 'paired' : 'idle');
      reply({ t: 'pairingResult', id, state: state.pairing });
      break;

    case 'startPairing':
      /* 配新主机：先断开当前主机，再发发现广播等新主机搜索（配完自动退出）。 */
      clearMockTimers();
      state.controller = null;
      setPlayerLed(0);
      startPairingFlow();
      reply({ t: 'pairingResult', id, state: 'scanning' });
      break;

    case 'unpair':
      clearMockTimers();
      bonded[bondKey()] = false;
      state.controller = null;
      setPlayerLed(0);
      // 凭证清空后回连与唤醒都失去目标：静默，等用户按连接键重新配对。
      setPairing('idle');
      reply({ t: 'unpairResult', id, state: 'idle' });
      break;

    case 'pressLr':
      // 浏览器预览没有双机配对流程，只回执成功供按钮反馈。
      reply({ t: 'pressLrAck', id, success: true });
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

    case 'powerOff':
      reply({ t: 'powerOffAck', id });
      // 浏览器里没有电源通路，按设备插着 USB 时的行为回报：锁存被旁路，
      // 固件在确认自己还活着之后告诉 UI 关不掉。
      later(1500, () => reply({ t: 'powerOffBlocked' }));
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

/* 上电不主动发信号（真机不按键不广播）：mock 里主机从未配过，开机静默等
 * 用户按连接键。 */
