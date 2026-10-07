// MCP 按键服务的工具面错误与校验：键名表、摇杆电平、数量上限。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

export const STICK_MAX = 4095;
export const STICK_CENTER = 2048;

/** 每次调用允许的按键数上限与 CLI 回复静默窗（镜像 session.handleLine 的取值）。 */
export const MAX_KEYS_PER_CALL = 8;
export const REPLY_SHORT_QUIET = 0.15;
export const REPLY_LONG_QUIET = 0.25;

/** 键名即固件调试注入的按键位名（pad_state.h 内部值，dp_source s_debug_keys）。 */
export const KEY_NAMES = [
  'circle', 'cross', 'triangle', 'square',
  'l1', 'r1', 'l4', 'r4', 'l3', 'r3',
  'up', 'down', 'left', 'right',
  'opt', 'touchpad', 'home', 'share', 'mute',
];

/** 脚本时间线的规模上限：事件数与默认总时长（超限拒绝执行）。 */
export const MAX_SCRIPT_ACTIONS = 512;
export const MAX_SCRIPT_EVENTS = 4096;
export const DEFAULT_SCRIPT_MAX_MS = 60000;
export const DEFAULT_TAP_MS = 200;
export const MAX_HOLD_MS = 60000;

/** 回放记录的规模上限：文件 4 MiB、展开事件 65536、默认总时长 10 分钟；缺省帧长 15ms。 */
export const MAX_REPLAY_BYTES = 4 * 1024 * 1024;
export const MAX_REPLAY_EVENTS = 65536;
export const DEFAULT_REPLAY_MAX_MS = 600000;
export const DEFAULT_FRAME_MS = 15.0;
/** 回放时间线的停止检查切片。 */
export const REPLAY_SLICE_S = 0.02;

/** 固件注入的单次保持上限（dp_source 钳制 60000ms）；无期限按住用更短的滚动期限。 */
export const HOLD_SLICE_MS = 55000;
export const HOLD_REFRESH_MS = 5000;

/** 工具面可预期失败：键名校验、参数越界、链路不可用等，直接透给 agent。 */
export class McpError extends Error {}

/** 键名列表校验：小写归一、去重保序、限制数量，返回规范键名。 */
export function validateKeys(keys, { allowEmpty = false, maxKeys = MAX_KEYS_PER_CALL } = {}) {
  if (!Array.isArray(keys) || (!keys.length && !allowEmpty)) {
    throw new McpError(`keys 必须是非空键名列表，可用键名：${KEY_NAMES.join(' ')}`);
  }
  const names = [];
  for (const key of keys) {
    if (typeof key !== 'string') {
      throw new McpError(`键名必须是字符串，收到 ${JSON.stringify(key)}`);
    }
    const name = key.trim().toLowerCase();
    if (!KEY_NAMES.includes(name)) {
      throw new McpError(`未知键名 ${JSON.stringify(key)}，可用键名：${KEY_NAMES.join(' ')}`);
    }
    if (!names.includes(name)) {
      names.push(name);
    }
  }
  if (names.length > maxKeys) {
    throw new McpError(`一次最多 ${maxKeys} 个键，收到 ${names.length} 个`);
  }
  return names;
}

/** 摇杆电平校验：0-4095 整数（2048 中位），越界或非整数报错。返回 [x, y]。 */
export function validateStickXy(x, y) {
  const values = [];
  for (const [name, raw] of [['x', x], ['y', y]]) {
    if (typeof raw === 'boolean' || typeof raw !== 'number' || !Number.isFinite(raw) || raw < 0 || raw > STICK_MAX) {
      throw new McpError(`摇杆 ${name} 必须是 0-${STICK_MAX} 的数值（${STICK_CENTER} 中位）`);
    }
    values.push(Math.round(raw));
  }
  return values;
}
