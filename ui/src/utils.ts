/**
 * 屏幕文案与格式化工具。所有中文与格式字符都以字面量出现在本文件，
 * 构建期据此烘焙字体图集；数字/冒号/百分号由 theme.ts 的锚点兜底。
 */
import type { PairingState } from './bridge/protocol';
import { COLOR } from './theme';

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

/**
 * 直插手柄的家族短名（固件上报的机读 token）→ 底栏标签。
 * 标签必须是本文件的字面量：固件回发的字符串没进字体图集，直接上屏会变豆腐块。
 * 空串（未接入）与 unknown（未登记型号按家族表回落 Xbox 布局）都落到通用标签。
 */
export function padFamilyLabel(name: string): string {
  switch (name) {
    case 'ps':
      return 'PS';
    case 'xbox':
      return 'XBOX';
    case 'ns':
      return 'NS';
    case 'steam':
      return 'STEAM';
    default:
      return 'PAD';
  }
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
