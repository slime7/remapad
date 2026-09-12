/**
 * Remapad 屏幕主题：Material Design 3 深色配色 token 与表面样式常量。
 *
 * PocketJS 的 Tailwind 子集按「整条 class 字符串」在构建期注册样式：运行时
 * 无法拼接出没出现过的组合，也无法在运行时改一个背景色变量。因此：
 * - 文字颜色经 style.textColor 引用 COLOR token（运行时取值，随状态切换）；
 * - 带背景色的表面（卡片、按钮、导航等）以完整 class 字面量收进 STYLE，
 *   颜色只在本文件出现，页面按语义引用，不再散落 hex。
 */

/** MD3 深色主题 token（用户提供），键名与 MD3 token 对应。 */
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
  surfaceContainerLowest: '#000000',
  surfaceContainerLow: '#081423',
  surfaceContainer: '#0c1a2c',
  surfaceContainerHigh: '#102035',
  surfaceContainerHighest: '#14263e',
  onSurface: '#d9e6ff',
  onSurfaceVariant: '#9aacca',
  outline: '#657692',
  outlineVariant: '#374962',
  /** 导航禁用态置灰（页面文字与 SVG 同源）。 */
  disabled: '#5b6a85',
  /** 按压态衍生色（只被本文件的 class 字面量引用）。 */
  pressedHigh: '#1c3350',
  pressedSecondary: '#4a2f40',
  pressedError: '#a02a2e',
  pressedPrimaryContainer: '#b8dbff',
  pressedTertiaryContainer: '#ffafd7',
  /** 半透明遮罩（状态栏 / 模态）。 */
  statusBarBg: '#081423b3',
  scrim: '#000000b3',
} as const;

/**
 * 表面样式常量：每条都是完整 class 字面量（构建期整体注册），供页面按
 * 语义复用；带 active: 前缀的按压色同样收在此处。
 */
export const STYLE = {
  /** 应用根容器与覆盖层。 */
  appRoot: 'w-full h-full relative bg-[#060f1b] overflow-hidden',
  statusBar: 'absolute top-0 left-0 right-0 h-[26] z-40 flex-row items-center px-6 gap-2 bg-[#081423b3]',
  scrim: 'absolute inset-0 z-50 flex-col items-center justify-center bg-[#000000b3]',
  busyOverlay: 'absolute inset-0 z-50 flex-row items-center justify-center bg-[#000000]',

  /** 模态对话框（240×280 本应用自绘）。 */
  modalBox: 'w-[204] rounded-[16] bg-[#102035] p-3 flex-col items-center',
  modalCancelBtn: 'w-[84] h-[40] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  modalDangerBtn: 'w-[84] h-[40] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150',

  /** 列表行与信息卡。 */
  rowCard: 'w-full h-[44] shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 active:bg-[#14263e] transition-colors duration-150',
  infoCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-2',
  actionCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-3',

  /** 小型表面按钮：背光步进、调试按键（含触发高亮的选中形态）。 */
  surfaceBtn: 'w-[40] h-[40] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  keyBtn: 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  keyBtnGrow: 'grow h-[44] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  keyBtnFull: 'w-full h-[44] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center mt-2 active:bg-[#1c3350] transition-colors duration-150',
  keyBtnOn: 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150',
  keyBtnGrowOn: 'grow h-[44] rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150',
  keyBtnFullOn: 'w-full h-[44] shrink-0 rounded-[12] bg-[#9ecefe] flex-row items-center justify-center mt-2 active:bg-[#b8dbff] transition-colors duration-150',

  /** 背光滑轨与危险操作行。 */
  backlightRow: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 py-2 gap-2',
  track: 'grow h-[8] rounded-[4] bg-[#14263e] overflow-hidden',
  trackFill: 'h-[8] rounded-[4] bg-[#9ecefe]',
  dangerRow: 'w-full h-[44] shrink-0 rounded-[16] bg-[#8a1a1e] flex-row items-center justify-center gap-2 active:bg-[#a02a2e] transition-colors duration-150',

  /** 首页状态圆与配对页大按钮。 */
  circle: 'w-[56] h-[56] rounded-full bg-[#14263e] flex-col items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  pairMain: 'w-[76] h-[76] rounded-full bg-[#9ecefe] flex-col items-center justify-center shrink-0 active:bg-[#b8dbff] transition-colors duration-150',
  pairMainStop: 'w-[76] h-[76] rounded-full bg-[#8a1a1e] flex-col items-center justify-center shrink-0 active:bg-[#a02a2e] transition-colors duration-150',
  pairAux: 'w-[76] h-[76] rounded-full bg-[#14263e] flex-col items-center justify-center shrink-0 active:bg-[#1c3350] transition-colors duration-150',

  /** 模式页/手柄设置页的选项卡（未选中 / 选中）。 */
  optionCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 py-3 gap-3 active:bg-[#14263e] transition-colors duration-150',
  optionCardSel: 'w-full shrink-0 rounded-[16] bg-[#9ecefe] flex-row items-center px-3 py-3 gap-3 active:bg-[#b8dbff] transition-colors duration-150',

  /**
   * 底部导航（5 键，SVG 图标）：两端的「状态 / 设置」贴近屏幕角落取
   * rounded-[24]，中间三键取 rounded-[16]；状态键沿用 secondary/tertiary
   * 家族配色，其余键沿用 blue 家族配色，禁用态只变前景色。
   */
  navHomeOn: 'w-[35] h-[64] shrink-0 rounded-[24] bg-[#fd9ace] flex-col items-center justify-center active:bg-[#ffafd7] transition-colors duration-150',
  navHomeOff: 'w-[35] h-[64] shrink-0 rounded-[24] bg-[#34202c] flex-col items-center justify-center active:bg-[#4a2f40] transition-colors duration-150',
  navMidOn: 'w-[35] h-[64] shrink-0 rounded-[16] bg-[#9ecefe] flex-col items-center justify-center active:bg-[#b8dbff] transition-colors duration-150',
  navMidOff: 'w-[35] h-[64] shrink-0 rounded-[16] bg-[#14263e] flex-col items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
  navCornerOn: 'w-[35] h-[64] shrink-0 rounded-[24] bg-[#9ecefe] flex-col items-center justify-center active:bg-[#b8dbff] transition-colors duration-150',
  navCornerOff: 'w-[35] h-[64] shrink-0 rounded-[24] bg-[#14263e] flex-col items-center justify-center active:bg-[#1c3350] transition-colors duration-150',
} as const;

/**
 * 构建期字符集锚点：uptime、百分比等文本在运行时由数字动态拼出，数字与
 * 符号必须出现在某个字面量里才会被烘焙进字体图集。
 */
export const CHARSET_ANCHOR = '0123456789:.%-';
