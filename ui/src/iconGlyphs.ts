/**
 * Material Icons 字形表（PUA 码点）：纯数据，不引入任何模块。
 *
 * 与渲染组件拆开是因为端到端用例要在 Node 侧读同一份字形取值，而 icons.tsx
 * 依赖 PocketJS 框架源码（node_modules 里的 .ts，Node 的加载器不处理）。
 * 码点只出现在本文件的字符串字面量里，构建期按需烘焙进各字号槽位。
 */
export const ICON = {
  home: '\ue88a',
  settings: '\ue8b8',
  bluetooth: '\ue1a7',
  bluetoothConnected: '\ue1a8',
  bluetoothDisabled: '\ue1a9',
  usb: '\ue1e0',
  usbOff: '\ue4fa',
  adb: '\ue60e',
  /** 电脑一对（同一台显示器）：desktop_windows e30c 与它的禁用形态
   *  desktop_access_disabled e99d（沿 desktop_windows 轮廓加一道斜线）。 */
  desktopWindows: '\ue30c',
  desktopAccessDisabled: '\ue99d',
  gamepad: '\ue30f',
  chevronRight: '\ue5cc',
  /** 翻页箭头（chevron_left / chevron_right）：四叶草左右凹陷处的无柄滑动提示。 */
  chevronLeft: '\ue5cb',
  power: '\ue8ac',
  swapHoriz: '\ue8d4',
  bug: '\ue868',
  brightnessHigh: '\ue1ac',
  add: '\ue145',
  remove: '\ue15b',
  /** 物理手柄连接状态图标（videogame_asset e338 / videogame_asset_off e500）。 */
  videogameAsset: '\ue338',
  videogameAssetOff: '\ue500',
  /** 主机连接图标（missing_controller e701）。 */
  missingController: '\ue701',
  /** 手柄操控底栏提示按键图标。 */
  gamepadLeft: '\ueecb',
  gamepadRight: '\ueeca',
  gameButtonL: '\ueede',
  gameButtonR: '\ueedb',
  gamepadUp: '\ueec9',
  gamepadDown: '\ueecc',
  gamepadCircleRight: '\ueece',
  gamepadCircleDown: '\ueed0',
  /** 电池分档只有经典四个字形（实心满电 / 通用 / 告警 / 充电），
   *  图集里没有按百分比分格的电量条，档位差由颜色区分（见 AppStatusBar）。 */
  batteryFull: '\ue1a4',
  batteryStd: '\ue1a5',
  batteryAlert: '\ue19c',
  batteryChargingFull: '\ue1a3',
} as const;
