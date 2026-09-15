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
 *
 * 可点表面一律带 focus: 白环：手柄操控模式下方向键移动焦点，原生核心直接
 * 套用 focus 变体，不需要每帧 JS 介入（见 docs/adr/0028）。环画在节点自身的
 * 边框层、又是 inset 的，所以只在子节点没铺满整块表面时才看得见——底栏按钮
 * 的贴图铺满整块，那里另加一层透明的焦点层（见 AppNavBar.tsx）。
 * 环写进每条 class 字面量而不是拼一个共用常量：构建期按字符串字面量登记样式，
 * 运行时拼出来的组合不在表里（设备上是抛错的未知 class）。
 */
export const STYLE = {
  /** 应用根容器与覆盖层。 */
  appRoot: 'w-full h-full relative bg-[#060f1b] overflow-hidden',
  statusBar: 'absolute top-0 left-0 right-0 h-[26] z-40 flex-row items-center px-6 gap-2 bg-[#081423b3]',
  scrim: 'absolute inset-0 z-50 flex-col items-center justify-center bg-[#000000b3]',
  busyOverlay: 'absolute inset-0 z-50 flex-row items-center justify-center bg-[#000000]',

  /** 模态对话框（240×280 本应用自绘）。 */
  modalBox: 'w-[204] rounded-[16] bg-[#102035] p-3 flex-col items-center',
  modalCancelBtn: 'w-[84] h-[40] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  modalDangerBtn: 'w-[84] h-[40] rounded-[12] bg-[#8a1a1e] flex-row items-center justify-center active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 列表行与信息卡。 */
  rowCard: 'w-full h-[44] shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 active:bg-[#14263e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  infoCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-2',
  /** 信息行卡：每行一个文本节点，行距由 gap 给出。 */
  infoCardRows: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-2 gap-2',
  actionCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-col px-3 py-3',

  /** 小型表面按钮：背光步进、调试按键（含触发高亮的选中形态）。 */
  surfaceBtn: 'w-[40] h-[40] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtn: 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtnGrow: 'grow h-[44] rounded-[12] bg-[#14263e] flex-row items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtnFull: 'w-full h-[44] shrink-0 rounded-[12] bg-[#14263e] flex-row items-center justify-center mt-2 active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtnOn: 'w-[64] h-[44] shrink-0 rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtnGrowOn: 'grow h-[44] rounded-[12] bg-[#9ecefe] flex-row items-center justify-center active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  keyBtnFullOn: 'w-full h-[44] shrink-0 rounded-[12] bg-[#9ecefe] flex-row items-center justify-center mt-2 active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 背光滑轨与危险操作行。 */
  backlightRow: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 py-2 gap-2',
  track: 'grow h-[8] rounded-[4] bg-[#14263e] overflow-hidden',
  trackFill: 'h-[8] rounded-[4] bg-[#9ecefe]',
  dangerRow: 'w-full h-[44] shrink-0 rounded-[16] bg-[#8a1a1e] flex-row items-center justify-center gap-2 active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 首页状态圆与配对页大按钮。 */
  circle: 'w-[56] h-[56] rounded-full bg-[#14263e] flex-col items-center justify-center active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  pairMain: 'w-[76] h-[76] rounded-full bg-[#9ecefe] flex-col items-center justify-center shrink-0 active:bg-[#b8dbff] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  pairMainStop: 'w-[76] h-[76] rounded-full bg-[#8a1a1e] flex-col items-center justify-center shrink-0 active:bg-[#a02a2e] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',
  pairAux: 'w-[76] h-[76] rounded-full bg-[#14263e] flex-col items-center justify-center shrink-0 active:bg-[#1c3350] transition-colors duration-150 focus:border-2 focus:border-[#ffffff]',

  /** 首页玩家序号四格指示灯（对齐 NS2 手柄的绿色序号灯）：点亮 / 未点亮。 */
  playerLedOn: 'w-[16] h-[16] rounded-[4] shrink-0 bg-[#4ade80]',
  playerLedOff: 'w-[16] h-[16] rounded-[4] shrink-0 bg-[#14432a]',

  /**
   * 手柄操控模式的提示条：抬起手柄、屏幕接管输入后浮在底栏上方，几秒后
   * 收起（见 components/PadControlHint.tsx）。深底加白边，压在任何页面内容
   * 上都读得清。
   */
  padHint: 'absolute left-[8] right-[8] bottom-[80] z-40 rounded-[12] bg-[#060f1bee] border border-[#657692] flex-col items-center px-3 py-2 gap-1',

  /**
   * 模式页/手柄设置页的选项卡（未选中 / 选中）。
   * 这两条不带 transition-colors：选中态要一步到位。加过渡后切换要连画 9 帧
   * （150 ms），每帧重画两张卡片（约 2.2 万像素，实机上每帧 11–14 ms），
   * 点下去像慢半拍。
   */
  optionCard: 'w-full shrink-0 rounded-[16] bg-[#0c1a2c] flex-row items-center px-3 py-3 gap-3 active:bg-[#14263e] focus:border-2 focus:border-[#ffffff]',
  optionCardSel: 'w-full shrink-0 rounded-[16] bg-[#9ecefe] flex-row items-center px-3 py-3 gap-3 active:bg-[#b8dbff] focus:border-2 focus:border-[#ffffff]',
} as const;

/**
 * 构建期字符集锚点：uptime、百分比等文本在运行时由数字动态拼出，数字与
 * 符号必须出现在某个字面量里才会被烘焙进字体图集；设备地址是运行时
 * 拼出的大写十六进制，字母表同样在这里锚定。
 */
export const CHARSET_ANCHOR = '0123456789:.%-ABCDEF';
