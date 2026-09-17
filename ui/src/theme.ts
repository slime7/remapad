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
  primary: '#b8dbff',
  onPrimary: '#154e77',
  primaryContainer: '#9ecefe',
  onPrimaryContainer: '#04456e',
  secondary: '#debece',
  onSecondary: '#503947',
  secondaryContainer: '#34202c',
  onSecondaryContainer: '#b99cab',
  tertiary: '#ffafd7',
  onTertiary: '#6f2251',
  tertiaryContainer: '#fd9ace',
  onTertiaryContainer: '#631847',
  error: '#ff716c',
  onError: '#490006',
  errorContainer: '#8a1a1e',
  onErrorContainer: '#ff9993',
  background: '#060f1b',
  onBackground: '#d9e6ff',
  surface: '#0c1a2c',
  onSurface: '#d9e6ff',
  surfaceVariant: '#14263e',
  onSurfaceVariant: '#9aacca',
  surfaceContainerLowest: '#000000',
  surfaceContainerLow: '#081423',
  surfaceContainer: '#0c1a2c',
  surfaceContainerHigh: '#102035',
  surfaceContainerHighest: '#14263e',
  inverseSurface: '#d9e6ff',
  inverseOnSurface: '#0c1a2c',
  inversePrimary: '#154e77',
  outline: '#657692',
  outlineVariant: '#374962',
  disabled: '#5b6a85',
} as const;

/**
 * 表面样式常量：每条都是完整 class 字面量（构建期整体注册），供页面按语义复用。
 * 可点表面一律带 focus: 白环：手柄操控模式下方向键移动焦点由原生核心直接呈现。
 */
export const STYLE = {
  /** 应用根容器与覆盖层。 */
  appRoot: 'w-full h-full relative bg-[#060f1b] overflow-hidden',
  scrim: 'absolute inset-0 z-50 flex-col items-center justify-center bg-[#000000b3]',
  busyOverlay: 'absolute inset-0 z-50 flex-row items-center justify-center bg-[#000000]',

  /** 模态对话框（240×280 本应用自绘）。 */
  modalBox: 'w-[204] rounded-[16] bg-[#102035] p-3 flex-col items-center',
  modalCancelBtn: 'w-[84] h-[40] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  modalDangerBtn: 'w-[84] h-[40] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 手柄配色圆钮：42 尺寸圆形色块与选中指示环。 */
  colorSwatch42: 'w-[42] h-[42] shrink-0 rounded-full flex-row items-center justify-center focus:border-2 focus:border-[#ffffff]',
  colorSwatchRing42: 'w-[36] h-[36] shrink-0 rounded-full border-2',

  /** 配对页操作按钮。 */
  pairMainBtn: 'w-[100] h-[36] rounded-[12] bg-[#04456e] flex-row items-center justify-center active:bg-[#081423] focus:border-2 focus:border-[#ffffff]',
  pairStopBtn: 'w-[100] h-[36] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  pairAuxBtn: 'w-[100] h-[32] rounded-[10] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 电源管理操作按钮。 */
  powerBtn: 'w-[104] h-[36] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  powerDangerBtn: 'w-[104] h-[36] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 调试页操作按钮。 */
  dbgBtn: 'w-[52] h-[32] rounded-[10] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgBtnOn: 'w-[52] h-[32] rounded-[10] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgFullBtn: 'w-[110] h-[32] rounded-[10] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  dbgFullBtnOn: 'w-[110] h-[32] rounded-[10] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 底部中区玩家指示灯：8px 微型方块。 */
  miniPlayerLedOn: 'w-[8] h-[8] rounded-[2] shrink-0 bg-[#4ade80]',
  miniPlayerLedOff: 'w-[8] h-[8] rounded-[2] shrink-0 bg-[#14432a]',

  /** 进度轨与填充（OTA 数据接收进度条）。 */
  track: 'grow h-[8] rounded-[4] bg-[#14263e] overflow-hidden',
  trackFill: 'h-[8] rounded-[4] bg-[#9ecefe]',

  /** 垂直亮度控制相关（四叶草 primaryContainer 背景上的嵌套组件）。 */
  vSliderTrack: 'w-[20] h-[96] rounded-[10] bg-[#04456e] flex-col justify-end p-1 overflow-hidden',
  vSliderFill: 'w-full rounded-[6] bg-[#ffffff]',
  brightnessBtn: 'w-[38] h-[38] rounded-[12] bg-[#0c1a2c] flex-row items-center justify-center active:bg-[#14263e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
} as const;

/**
 * 构建期字符集锚点：保证运行时动态拼接的数字、状态字符进入字体图集。
 */
export const CHARSET_ANCHOR = '0123456789:.%-ABCDEF左右上下选择确认长按退出翻页未连接接收中';
