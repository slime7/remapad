// 固件设置回读行的解析：图形界面用它把设备回读同步进设置控件，
// 固件是唯一事实源，界面不自算状态。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

/** 把一行回读按空白切开，收下所有 `键=值` 片段；值里带空格时只留第一段。 */
function replyFields(line) {
  const fields = {};
  for (const token of line.split(/\s+/)) {
    if (!token) {
      continue;
    }
    const eq = token.indexOf('=');
    if (eq > 0) {
      const key = token.slice(0, eq);
      if (!(key in fields)) {
        fields[key] = token.slice(eq + 1);
      }
    }
  }
  return fields;
}

function intField(text) {
  if (!text) {
    return null;
  }
  if (!/^[+-]?\d+$/.test(text.trim())) {
    return null;
  }
  return Number.parseInt(text, 10);
}

/** `0xRRGGBB` → 整数；不是合法的一段配色返回 null（界面对应项保持原值）。 */
function hexField(text) {
  if (!text) {
    return null;
  }
  if (!/^0x[0-9a-f]+$/i.test(text)) {
    return null;
  }
  const value = Number.parseInt(text, 16);
  return value >= 0 && value <= 0xffffff ? value : null;
}

/** `458752/524288` → [空闲字节, 总量字节]；老固件只报空闲时总量为 null。 */
function heapField(text) {
  const free = intField(text);
  if (free !== null) {
    return [free, null];
  }
  if (!text || !text.includes('/')) {
    return null;
  }
  const [freeText, totalText] = text.split('/', 2);
  const free2 = intField(freeText);
  const total = intField(totalText);
  return free2 !== null && total !== null ? [free2, total] : null;
}

/** `3971mV/85%` → [毫伏, 百分比]。 */
function batteryField(text) {
  if (!text || !text.includes('/')) {
    return null;
  }
  let [millivolts, percent] = text.split('/', 2);
  if (millivolts.endsWith('mV')) {
    millivolts = millivolts.slice(0, -2);
  }
  if (percent.endsWith('%')) {
    percent = percent.slice(0, -1);
  }
  const mv = intField(millivolts);
  const pct = intField(percent);
  return mv !== null && pct !== null ? [mv, pct] : null;
}

/**
 * 识别固件设置回读行 → {channel, fields}；认不出的行返回 null。
 * 通道与字段：
 *
 * - `backlight`：`backlight 60` → `light`；
 * - `screen`：`screen on|off` → `screen_on`；
 * - `ctrl`：`ok ctrl body=0x… button=0x… accent=0x… grip=0x…` → 四段 `0xRRGGBB`；
 * - `ds`：`ds touchpad=on|off capture=on|off` → `touchpad_plus_minus` / `capture_key`；
 * - `netlog`：`netlog state=… ssid=… dest=…` → 会话状态（state / ssid / dest）；
 * - `netlog_cred`：`netlog cred ssid=… pass=…` → 已存凭据回读（未配置时两个都是空串）；
 * - `device`：`status` 与 `version` 的一行回读 → 版本、分区、电池、堆与配对等事实，
 *   固件字段名收敛成 `firmware` / `partition` / `image` / `ota_state` /
 *   `pairing` / `role` / `pad` / `light` / `screen_on` / `battery_mv` /
 *   `battery_percent` / `charging` / `heap`（空闲字节）/ `heap_total` / `uptime_s`。
 *
 * 带状态词的应答先剥前缀：`err` 行大多答不进这里的字段表，自然返回 null。
 */
export function parseDeviceReply(line) {
  let text = line.trim();
  for (const status of ['ok ', 'err ']) {
    if (text.startsWith(status)) {
      text = text.slice(status.length).trim();
      break;
    }
  }
  if (!text) {
    return null;
  }
  const words = text.split(/\s+/);
  if (words[0] === 'backlight' && words.length > 1) {
    const light = intField(words[1]);
    if (light !== null && light >= 0 && light <= 100) {
      return { channel: 'backlight', fields: { light } };
    }
    return null;
  }
  if (words[0] === 'screen' && words.length > 1) {
    if (words[1] === 'on' || words[1] === 'off') {
      return { channel: 'screen', fields: { screen_on: words[1] === 'on' } };
    }
    return null;
  }
  const fields = replyFields(text);
  if (words[0] === 'ctrl' && ['body', 'button', 'accent', 'grip'].every((key) => key in fields)) {
    const colors = {};
    for (const key of ['body', 'button', 'accent', 'grip']) {
      const value = hexField(fields[key]);
      if (value === null) {
        return null;
      }
      colors[key] = value;
    }
    return { channel: 'ctrl', fields: colors };
  }
  if (words[0] === 'ds') {
    const touchpad = fields.touchpad;
    const capture = fields.capture;
    if ((touchpad === 'on' || touchpad === 'off') && (capture === 'on' || capture === 'off')) {
      return { channel: 'ds', fields: { touchpad_plus_minus: touchpad === 'on', capture_key: capture === 'on' } };
    }
    return null;
  }
  if (words[0] === 'netlog' && words.length > 1 && words[1] === 'cred') {
    // netlog cred ssid=slime_nest pass=hunter2（未配置时固件报 ssid=- pass=-，换成空串）
    if (!('ssid' in fields)) {
      return null;
    }
    return {
      channel: 'netlog_cred',
      fields: {
        ssid: fields.ssid === '-' ? '' : fields.ssid,
        pass: (fields.pass ?? '-') === '-' ? '' : fields.pass,
      },
    };
  }
  if (words[0] === 'netlog') {
    // netlog state=connected ssid=slime_nest dest=192.168.1.5:9999 …
    const state = fields.state;
    if (state) {
      return { channel: 'netlog', fields: { state, ssid: fields.ssid ?? '-', dest: fields.dest ?? '-' } };
    }
    return null;
  }
  if (words[0] === 'state' || text.startsWith('fw=')) {
    const facts = {};
    for (const [key, name] of [
      ['fw', 'firmware'], ['part', 'partition'], ['image', 'image'],
      ['ota', 'ota_state'], ['pairing', 'pairing'], ['role', 'role'], ['pad', 'pad'],
    ]) {
      if (key in fields) {
        facts[name] = fields[key];
      }
    }
    const light = intField(fields.backlight);
    if (light !== null) {
      facts.light = light;
    }
    const heap = heapField(fields.heap);
    if (heap !== null) {
      facts.heap = heap[0];
      if (heap[1] !== null) {
        facts.heap_total = heap[1];
      }
    }
    if (fields.screen === '0' || fields.screen === '1') {
      facts.screen_on = fields.screen === '1';
    }
    if (fields.chg === '0' || fields.chg === '1') {
      facts.charging = fields.chg === '1';
    }
    const uptime = intField((fields.uptime ?? '').replace(/s$/, ''));
    if (uptime !== null) {
      facts.uptime_s = uptime;
    }
    const battery = batteryField(fields.batt);
    if (battery !== null) {
      facts.battery_mv = battery[0];
      facts.battery_percent = battery[1];
    }
    return Object.keys(facts).length ? { channel: 'device', fields: facts } : null;
  }
  return null;
}
