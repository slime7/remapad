import type { BatteryInfo, ControllerMode, ControllerModel, DeviceCmd, DeviceMsg, PairingState } from './protocol';

interface MockHardwareState {
  battery: BatteryInfo;
  backlight: number;
  mode: ControllerMode;
  pairing: PairingState;
  controller: ControllerModel;
}

const state: MockHardwareState = {
  battery: {
    voltageMv: 4120,
    percentage: 88,
    charging: false,
  },
  backlight: 80,
  mode: 'ble',
  pairing: 'connected',
  controller: 'pro-controller-2',
};

/**
 * 浏览器仿真环境下的虚拟外设响应分发器
 */
export function mockHandleCmd(cmd: DeviceCmd, reply: (msg: DeviceMsg) => void): void {
  const id = cmd.id;

  switch (cmd.t) {
    case 'hello':
      reply({
        t: 'ready',
        id,
        chip: 'ESP32-S3 (Browser Wasm Mock)',
        firmwareVersion: 'v0.1.0-sim',
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
        t: 'pairingResult',
        id,
        state: state.pairing,
        message: `Mode switched to ${cmd.mode}`,
      });
      break;

    case 'startPairing':
      state.pairing = 'pairing';
      reply({
        t: 'pairingResult',
        id,
        state: 'pairing',
        message: 'Pairing broadcast active',
      });
      break;

    case 'stopPairing':
      state.pairing = 'idle';
      reply({
        t: 'pairingResult',
        id,
        state: 'idle',
        message: 'Pairing stopped',
      });
      break;

    case 'triggerRumble':
      reply({
        t: 'rumbleAck',
        id,
        success: true,
      });
      break;

    case 'calibrateSensors':
    case 'reboot':
      reply({
        t: 'ready',
        id,
        chip: 'ESP32-S3 (Reboot Sim)',
        firmwareVersion: 'v0.1.0-sim',
        psramSize: 8 * 1024 * 1024,
      });
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
