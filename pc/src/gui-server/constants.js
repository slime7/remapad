// GUI 前后端共用界面常量：快捷动作、命令分组、配色预设与状态灯文案。
// 后端用于装配与守卫，前端经 @remapad/pc/gui-constants 引用同一份。

export const STATE_STYLE = {
  disconnected: { label: '● 未连接', color: '#8a8a8a' },
  connecting: { label: '● 连接中', color: '#d7a63b' },
  connected: { label: '● 已连接', color: '#3fa66a' },
  broken: { label: '● 链路断开', color: '#e06c75' },
};

/** 快捷操作按钮：(按钮文字, 发到设备的命令)。 */
export const QUICK_ACTIONS = [
  ['配新主机', 'pairing start'],
  ['停止广播', 'pairing stop'],
  ['唤醒主机', 'wake'],
  ['断开主机', 'drop'],
  ['实机截图', ':shot'],
];

/** 命令页的分组：点击只填进输入框，回车才发送。 */
export const COMMAND_GROUPS = [
  ['输入注入', ['key circle 200', 'key release', 'stick reset', 'rumble off', 'lamp 0xF', 'haptic 0x10']],
  ['屏幕与连接', ['connect', 'ui on', 'ui off', 'backlight 60', 'screen off', 'beep']],
  ['诊断与状态', ['status', ':all', 'pad', 'link', 'mode host', 'rollback', ':log 15', ':help']],
];

/** 设置页的配色预设：与 UI 手柄设置页的四款一致。 */
export const COLORWAYS = [
  ['标准黑', 0x232323, 0xA0A0A0, 0xE6E6E6, 0x323232],
  ['枪灰黑', 0x3A4045, 0x9AA3AB, 0xC8CDD2, 0x2B2F33],
  ['银灰', 0xB9BEC4, 0x6E757C, 0xE6E6E6, 0x8A9096],
  ['墨绿金', 0x1E3B2A, 0xC8A24A, 0xC8A24A, 0x16301F],
];

/** 四段配色的字段顺序（机身 / 按键 / 高光 / 握把，与固件 ctrl 命令一致）。 */
export const COLOR_FIELDS = ['机身', '按键', '高光', '握把'];

/** 出厂占位配色：没读过设备前填在自定义输入框里（与固件默认值一致）。 */
export const DEFAULT_COLORS = [0x232323, 0xA0A0A0, 0xE6E6E6, 0x323232];

/** 读设置的回读命令：连上设备与点「读取当前设置」时各发一遍。 */
export const SETTINGS_READ_COMMANDS = ['status', 'version', 'ctrl', 'ds', 'netlog', 'netlog cred'];
