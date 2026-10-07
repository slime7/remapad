// 图形界面与工具共用的纯展示逻辑：不碰链路与控件，前后端共用同一份。
// 文案与拼行规则见 pc/README.md。

/** 工具条里手柄摘要的显示上限：完整描述留给「会话」页的下拉（长名字会顶掉工具条）。 */
export const PAD_SUMMARY_CHARS = 26;

/** 设备端 UDP 调试通道的默认端口（netlog）：桥接帧、CLI 文本与日志同一端口。 */
export const NETLOG_PORT_DEFAULT = 9999;

/** 亮度滑条范围：固件接受 0-100，0 只在息屏时出现，滑条下限留到 5。 */
export const BRIGHTNESS_MIN = 5;
export const BRIGHTNESS_MAX = 100;

/** 回读值的短标：固件给的是枚举串，界面换成中文。 */
export const PAIRING_TEXT = {
  idle: '未配对',
  scanning: '扫描中',
  advertising: '连接中',
  pairing: '配对中',
  paired: '已配对',
  connected: '已连接',
  error: '配对出错',
};
export const IMAGE_TEXT = { confirmed: '已确认', 'pending-verify': '待验证' };
export const ROLE_TEXT = { device: '串口', host: 'USB 主机' };

/** 工具条里的手柄摘要：太长就按词截断，完整描述留在「会话」页的下拉里。 */
export function shortPadName(description, limit = PAD_SUMMARY_CHARS) {
  if (description.length <= limit) {
    return description;
  }
  const head = description.slice(0, limit);
  const cut = head.lastIndexOf(' ');
  return (cut > 0 ? head.slice(0, cut) : head) + '…';
}

/** 串口列表的一行摘要：标出当前选中的那个，选中项不在列表里也直说。 */
export function portSummary(ports, current) {
  if (!ports.length) {
    return '没有检测到串口（设备挂的是 USB-Serial/JTAG，插上后点「刷新」）';
  }
  const listed = ports.map((port) => (port === current ? `${port}（已选）` : port)).join('  ');
  if (current && !ports.includes(current)) {
    return `串口 ${ports.length} 个：${listed}；当前选的 ${current} 不在列表里`;
  }
  return `串口 ${ports.length} 个：${listed}`;
}

/** 刷新后该选中哪个串口：用户自己选过就守着他的选择，否则落在本机第一个口上。
 *
 * 本机一个口都没有时保留手里的值（可能是马上要插上的板子，也可能是手动敲的）；
 * 用户清空过输入框就留空，让他自己填。
 */
export function portSelection(ports, current, fallback, chosen) {
  if (chosen) {
    return current;
  }
  return ports[0] ?? (current || fallback);
}

/** 0xRRGGBB → #rrggbb（配色按钮的底色用）。 */
export function cssColor(rgb) {
  return `#${(rgb & 0xffffff).toString(16).padStart(6, '0')}`;
}

/** 按亮度挑前景色：亮底黑字、暗底白字（与 UI 配色块的描边判定同一条）。 */
export function readableOn(rgb) {
  const red = (rgb >> 16) & 0xff;
  const green = (rgb >> 8) & 0xff;
  const blue = rgb & 0xff;
  return 0.2126 * red + 0.7152 * green + 0.0722 * blue > 0.4 * 255 ? '#000000' : '#ffffff';
}

/** 配色按钮的悬停色：整体压暗一档，按钮不至于看起来是死的。 */
export function pressedColor(rgb, factor = 0.85) {
  const channels = [16, 8, 0].map((shift) => {
    const value = Math.round((((rgb >> shift) & 0xff) * factor));
    return Math.max(0, Math.min(255, value));
  });
  return `#${channels.map((c) => c.toString(16).padStart(2, '0')).join('')}`;
}

/** 输入框里的 0xRRGGBB / RRGGBB → 整数；不是一段合法配色返回 null。 */
export function parseColor(text) {
  const cleaned = text.trim().toLowerCase();
  const body = cleaned.startsWith('0x') ? cleaned.slice(2) : cleaned;
  if (!body || body.length > 6 || !/^[0-9a-f]+$/.test(body)) {
    return null;
  }
  return Number.parseInt(body, 16);
}

/** 开机时长 → 时:分:秒；不足一小时只给 分:秒。 */
export function formatUptime(seconds) {
  const total = Math.max(seconds, 0);
  const hours = Math.floor(total / 3600);
  const minutes = Math.floor((total % 3600) / 60);
  const secs = total % 60;
  const mmss = `${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
  return hours ? `${hours}:${mmss}` : mmss;
}

/** 设备事实拼成两行：没读到的项不占位。
 *
 * 堆内存按设备屏幕的系统信息页同口径（内部堆的已用 / 总量 KB），空闲字节由回读给出、
 * 总量缺省（老固件）时退回到空闲值。电量与端电压不上屏：设备已放弃自带电源管理，
 * 这两项只在事实数据里保留（info 等命令仍可读）。
 */
export function formatDeviceFacts(facts) {
  const image = String(facts.image ?? '');
  const first = [
    facts.firmware ? `固件 ${facts.firmware}` : '',
    facts.partition ? `分区 ${facts.partition}` : '',
    image ? `镜像 ${IMAGE_TEXT[image] ?? image}` : '',
    facts.ota_state ? `升级 ${facts.ota_state}` : '',
  ];
  const second = [];
  if (facts.heap != null) {
    const freeKb = Math.floor(Number(facts.heap) / 1024);
    const total = facts.heap_total;
    if (total == null) {
      second.push(`堆内存 空闲 ${freeKb} KB`);
    } else {
      const usedKb = Math.floor((Number(total) - Number(facts.heap)) / 1024);
      const totalKb = Math.floor(Number(total) / 1024);
      second.push(`堆内存 ${usedKb} / ${totalKb} KB`);
    }
  }
  if (facts.uptime_s != null) {
    second.push(`运行 ${formatUptime(Number(facts.uptime_s))}`);
  }
  const pairing = String(facts.pairing ?? '');
  if (pairing) {
    second.push(`配对 ${PAIRING_TEXT[pairing] ?? pairing}`);
  }
  const role = String(facts.role ?? '');
  if (role) {
    second.push(`角色 ${ROLE_TEXT[role] ?? role}`);
  }
  if (facts.pad && facts.pad !== 'none') {
    second.push(`手柄 ${facts.pad}`);
  }
  const lines = [first, second].map((line) => line.filter(Boolean).join(' ｜ '));
  return lines.filter(Boolean).join('\n') || '设备没有回可读的状态';
}
