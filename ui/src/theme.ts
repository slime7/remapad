/**
 * Remapad 屏幕主题：Material Design 3 深色配色 token 与表面样式常量。
 *
 * PocketJS 的 Tailwind 子集按「整条 class 字符串」在构建期注册样式：运行时
 * 无法拼接出没出现过的组合，也无法在运行时改一个背景色变量。因此：
 * - 文字颜色经 style.textColor 引用 COLOR token（运行时取值，随状态切换）；
 * - 带背景色的表面（卡片、按钮、导航等）以完整 class 字面量收进 STYLE，
 *   颜色只在本文件出现，页面按语义引用，不再散落 hex。
 * 样式按整条字面量注册，STYLE 里的长 class 字符串因此不折行，其中数条超过
 * .editorconfig 里的 120 字符行宽约定。
 */

/** MD3 深色主题语义 token（键名与 Material Design 3 token 对应）。 */
export const COLOR = {
  primary: '#7e9fd4',
  primaryDim: '#5476a8',
  onPrimary: '#002043',
  primaryContainer: '#a6c8ff',
  onPrimaryContainer: '#1b416f',
  primaryFixed: '#a6c8ff',
  primaryFixedDim: '#98baf0',
  onPrimaryFixed: '#002c58',
  onPrimaryFixedVariant: '#264a79',
  secondary: '#8ca394',
  secondaryDim: '#637a6c',
  onSecondary: '#0f241a',
  secondaryContainer: '#152a1f',
  onSecondaryContainer: '#92a99a',
  secondaryFixed: '#def7e6',
  secondaryFixedDim: '#d0e8d8',
  onSecondaryFixed: '#374d40',
  onSecondaryFixedVariant: '#53695c',
  tertiary: '#4eb079',
  tertiaryDim: '#1a8552',
  onTertiary: '#002613',
  tertiaryContainer: '#9afdbf',
  onTertiaryContainer: '#006239',
  tertiaryFixed: '#9afdbf',
  tertiaryFixedDim: '#8ceeb1',
  onTertiaryFixed: '#004e2c',
  onTertiaryFixedVariant: '#006e40',
  error: '#ff716c',
  errorDim: '#c94947',
  onError: '#490006',
  errorContainer: '#8a1a1e',
  onErrorContainer: '#ff9993',
  background: '#060e1b',
  onBackground: '#d9e6ff',
  surface: '#060e1b',
  surfaceDim: '#060e1b',
  surfaceBright: '#192d48',
  surfaceContainerLowest: '#000000',
  surfaceContainerLow: '#091423',
  surfaceContainer: '#0d1a2c',
  surfaceContainerHigh: '#112035',
  surfaceContainerHighest: '#15263e',
  onSurface: '#d9e6ff',
  surfaceVariant: '#15263e',
  onSurfaceVariant: '#9bacca',
  outline: '#667692',
  outlineVariant: '#384962',
  inverseSurface: '#f9f9ff',
  inverseOnSurface: '#4d5564',
  inversePrimary: '#3e6090',
  shadow: '#000000',
  scrim: '#000000',
  surfaceTint: '#7e9fd4',
  disabled: '#53695c',
} as const;

/**
 * 表面样式常量：每条都是完整 class 字面量（构建期整体注册），供页面按语义复用。
 * 可点表面一律带 focus: 白环：手柄操控模式下方向键移动焦点由原生核心直接呈现。
 */
export const STYLE = {
  /** 应用根容器与覆盖层。 */
  appRoot: 'w-full h-full relative bg-[#060e1b] overflow-hidden',
  scrim: 'absolute inset-0 z-50 flex-col items-center justify-center bg-[#000000b3]',
  busyOverlay: 'absolute inset-0 z-50 flex-row items-center justify-center bg-[#000000]',

  /** 模态对话框（240×280 本应用自绘）。 */
  modalBox: 'w-[204] rounded-[16] bg-[#112035] p-3 flex-col items-center',
  modalCancelBtn: 'w-[84] h-[40] rounded-[12] bg-[#15263e] flex-row items-center justify-center active:bg-[#192d48] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  modalDangerBtn: 'w-[84] h-[40] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 手柄配色圆钮：42 尺寸圆形色块与选中指示环。 */
  colorSwatch42: 'w-[42] h-[42] shrink-0 rounded-full flex-row items-center justify-center focus:border-2 focus:border-[#ffffff]',
  colorSwatchRing42: 'w-[36] h-[36] shrink-0 rounded-full border-2',

  /** 配对页操作按钮。 */
  pairMainBtn: 'w-[100] h-[36] rounded-[12] bg-[#1b416f] flex-row items-center justify-center active:bg-[#091423] focus:border-2 focus:border-[#ffffff]',
  pairStopBtn: 'w-[100] h-[36] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  pairAuxBtn: 'w-[100] h-[32] rounded-[10] bg-[#15263e] flex-row items-center justify-center active:bg-[#192d48] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 电源管理操作按钮。 */
  powerBtn: 'w-[104] h-[36] rounded-[12] bg-[#15263e] flex-row items-center justify-center active:bg-[#192d48] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  powerDangerBtn: 'w-[104] h-[36] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 调试页操作按钮。 */
  dbgBtn: 'w-[52] h-[32] rounded-[10] bg-[#15263e] flex-row items-center justify-center active:bg-[#192d48] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgBtnOn: 'w-[52] h-[32] rounded-[10] bg-[#a6c8ff] flex-row items-center justify-center active:bg-[#7e9fd4] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgFullBtn: 'w-[110] h-[32] rounded-[10] bg-[#15263e] flex-row items-center justify-center active:bg-[#192d48] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgFullBtnOn: 'w-[110] h-[32] rounded-[10] bg-[#a6c8ff] flex-row items-center justify-center active:bg-[#7e9fd4] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 底部中区玩家指示灯：8px 微型方块（底栏为 tertiary #4eb079，点亮用 primaryContainer #a6c8ff，熄灭用 onTertiary #002613）。 */
  miniPlayerLedOn: 'w-[8] h-[8] rounded-[2] shrink-0 bg-[#a6c8ff]',
  miniPlayerLedOff: 'w-[8] h-[8] rounded-[2] shrink-0 bg-[#002613]',

  /** 进度轨与填充（OTA 数据接收进度条）：轨道给显式宽度并压暗——
   *  items-center 的列容器里 grow 撑不开宽度，轨道会塌成填充的尺寸。 */
  track: 'w-[192] h-[8] rounded-[4] bg-[#00261359] overflow-hidden',
  trackFill: 'h-[8] rounded-[4] bg-[#002613]',

  /** 垂直亮度控制相关（四叶草 primaryContainer 背景上的嵌套组件）。 */
  vSliderTrack: 'w-[24] h-[120] rounded-[12] bg-[#1b416f] flex-col justify-end p-1 overflow-hidden',
  vSliderFill: 'w-full rounded-[8] bg-[#ffffff]',
  brightnessBtn: 'w-[42] h-[42] rounded-[12] bg-[#0d1a2c] flex-row items-center justify-center active:bg-[#15263e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
} as const;

/**
 * 构建期字符集锚点：保证运行时动态拼接的数字、状态字符进入字体图集。
 */
export const CHARSET_ANCHOR = '0123456789:.%-ABCDEF左右上下选择确认长按退出翻页未连接接收中⠁⠂⠄⡀⢀⠠⠐⠈';
