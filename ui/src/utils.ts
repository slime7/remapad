/**
 * 屏幕文案与格式化工具。所有中文与格式字符都以字面量出现在本文件，
 * 构建期据此烘焙字体图集；数字/冒号/百分号由 theme.ts 的锚点兜底。
 */
import type { PairingState, UsbRole } from './bridge/protocol';
import { COLOR } from './theme';

/** USB 链路展示状态：off=没插 / adb=PC+烧录（串口） / computer=PC+OTG / gamepad=手柄+主机。 */
export type UsbLinkState = 'off' | 'adb' | 'computer' | 'gamepad';

/** 由控制面的角色与生效标志推导 USB 链路状态；串口（device）对应烧录态 adb。 */
export function usbLinkState(usbRole: UsbRole, usbRoleActive: boolean): UsbLinkState {
  if (!usbRoleActive) {
    return 'off';
  }
  if (usbRole === 'host') {
    return 'gamepad';
  }
  return usbRole === 'otg' ? 'computer' : 'adb';
}

/** 状态栏的 USB 角色短标。 */
export function usbRoleLabel(usbRole: UsbRole): string {
  switch (usbRole) {
    case 'host':
      return 'HOST';
    case 'otg':
      return 'OTG';
    default:
      return 'COM';
  }
}

/** 开机时长 → mm:ss 或 h:mm:ss。 */
export function formatUptime(ms: number): string {
  const total = Math.floor(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const mm = `${Math.floor(m / 10)}${m % 10}`;
  const ss = `${Math.floor(s / 10)}${s % 10}`;
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`;
}

/** 字节数 → MB 文本（一位小数，如 8.0 MB）。 */
export function formatMb(bytes: number): string {
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

/** 配对状态中文标签。 */
export function pairingLabel(state: PairingState): string {
  switch (state) {
    case 'scanning':
      return '扫描中…';
    case 'advertising':
      return '连接中…';
    case 'pairing':
      return '配对中…';
    case 'paired':
      return '已配对';
    case 'connected':
      return '已连接';
    case 'error':
      return '配对出错';
    default:
      return '未配对';
  }
}

/**
 * 配对页与模式页的提示文本：只允许本仓库 ui/src 里的字面量。
 * 构建期字体字符集按源码字面量扫描烘焙，固件回发的诊断文本直接上屏会显示
 * 成豆腐块——联合类型把这条规则钉在类型上。
 */
export type PairingNotice =
  | ''
  | '广播中，等待主机连接'
  | '已打开连接，等待主机连回来'
  | '已断开连接'
  | '已停止广播'
  | '已解除配对'
  | '当前状态无法执行 LR 配对'
  | '连接命令未生效'
  | '停止命令未生效'
  | '配对命令未生效'
  | '重启中…';

/** USB 角色切换提示：同 PairingNotice，必须是 ui/src 里的字面量。 */
export type RoleNotice = '' | 'USB host 数据面未接入，切换暂不生效' | 'USB 角色切换未生效';

