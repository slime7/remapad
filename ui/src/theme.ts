/**
 * Remapad 屏幕主题：Material Design 3 深色配色 token。
 *
 * PocketJS 的 Tailwind 子集在构建期扫描源码字符串字面量并烘焙样式，所以
 * class 只能以完整字面量出现在使用处；颜色烘成 ABGR 后没有运行时变量
 * 机制。这里集中放 hex 常量供图标填充色和少量 style 对象复用。
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
} as const;

/**
 * 构建期字符集锚点：uptime、百分比等文本在运行时由数字动态拼出，数字与
 * 符号必须出现在某个字面量里才会被烘焙进字体图集。
 */
export const CHARSET_ANCHOR = '0123456789:.%-';
