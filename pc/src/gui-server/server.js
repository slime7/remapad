// Remapad 连接控制台后端：装配串口/网络会话，HTTP REST + WS 推流给浏览器前端。
// 界面状态全部来自这里与固件回读行；链路与协议逻辑复用 session/link，不在这里复刻。
// 用法与 API 面见 pc/README.md。

import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

import { DEFAULT_IMAGE, parseCtrlArgs } from '../args.js';
import { openHint, listSerialPorts, SerialLink } from '../link/serial.js';
import { parseEndpoint, UdpLink } from '../link/net.js';
import { maskSecrets } from '../link/secrets.js';
import { describe, HidUnavailable, listCandidates, loadHid } from '../input/pads.js';
import { Session, waitForVersion } from '../session/session.js';
import { ImageError, loadImage } from '../session/image.js';
import { parseDeviceReply } from '../session/device-reply.js';
import { formatDeviceFacts, portSelection, portSummary, shortPadName } from '../gui-shared.js';
import { DEFAULT_COLORS, SETTINGS_READ_COMMANDS } from './constants.js';

/** 日志环形缓冲上限：WS 客户端刚连上时回放最近一段，不用刷新补历史。 */
export const RECENT_LIMIT = 400;

const MIME_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.map': 'application/json',
  '.woff2': 'font/woff2',
};

/** 用系统看图器打开文件（仅 Windows）：后端与设备同机，等价 Python 的 os.startfile。 */
function openWithShell(filePath) {
  spawn('cmd', ['/c', 'start', '', filePath], { detached: true, stdio: 'ignore' }).unref();
}

export class GuiServer {
  constructor(options = {}) {
    this.args = options.args ?? parseCtrlArgs([]);
    this.args.image = DEFAULT_IMAGE;
    this.createSerial = options.createSerial ?? ((port, baud) => new SerialLink(port, baud));
    this.createUdp = options.createUdp ?? ((host, port) => new UdpLink(host, port));
    this.openPath = options.openPath ?? openWithShell;
    this.Session = options.sessionClass ?? Session;
    this.waitForVersion = options.waitForVersion ?? waitForVersion;
    this.loadHidFn = options.loadHid ?? loadHid;
    this.listPorts = options.listPorts ?? listSerialPorts;
    /** 会话记录出口：cli.js 接 WS 广播；测试注入收集器。record(record) 必填。 */
    this.record = options.record ?? (() => {});

    this.hid = null;
    this.hidProblem = '';
    this.padEntries = [];
    this.padIndex = null;
    this.ports = [];
    this.port = this.args.port;
    this.portChosen = false;
    this.session = null;
    this.link = null;
    this.netSession = null;
    this.sessionState = 'disconnected';
    this.stateText = '未连接';
    this.lastError = '';
    this.pendingOtaWait = false;
    this.otaWait = true;
    this.otaPercent = 0;
    this.otaLabel = '未开始';
    this.recent = [];
    this.facts = {};
    // 设置控件值全部来自固件回读行，这里只存展示态；设备是唯一事实源。
    this.settings = {
      brightness: 60,
      screenOn: true,
      colors: [...DEFAULT_COLORS],
      colorSummary: '当前：未读取',
      dsTouchpad: false,
      dsCapture: true,
      wifiSsid: '',
      wifiPass: '',
      netSummary: '设备：未读取',
    };
  }

  // --- 会话记录 --------------------------------------------------

  /** 一条会话记录：脱敏后进环形缓冲并广播。 */
  emit(record) {
    if (record.text !== undefined) {
      record.text = maskSecrets(record.text);
    }
    this.recent.push(record);
    if (this.recent.length > RECENT_LIMIT) {
      this.recent.splice(0, this.recent.length - RECENT_LIMIT);
    }
    this.record(record);
  }

  log(text) {
    this.emit({ kind: 'line', text });
  }

  logError(text) {
    this.lastError = text;
    this.emit({ kind: 'error', text });
  }

  // --- 状态快照 --------------------------------------------------

  get connected() {
    return this.session !== null;
  }

  snapshot() {
    const padDescribe = this.session?.padInfo ? describe(this.session.padInfo) : null;
    const session = this.session;
    return {
      sessionState: this.sessionState,
      stateText: this.stateText,
      lastError: this.lastError,
      connected: this.connected,
      netSession: this.netSession !== null,
      forward: session ? session.forward : true,
      defaultImage: DEFAULT_IMAGE,
      ports: this.ports,
      port: this.port,
      pads: this.padEntries.map((info) => ({ describe: describe(info), path: info.path })),
      padIndex: this.padIndex,
      hidReady: this.hid !== null,
      hidProblem: this.hidProblem,
      padSummary: padDescribe ? shortPadName(padDescribe) : '手柄：未接入',
      counters: session
        ? { reports: session.reports, frames: session.frames, outputs: session.outputs }
        : { reports: 0, frames: 0, outputs: 0 },
      ota: { percent: this.otaPercent, label: this.otaLabel },
      facts: this.facts,
      factsText: formatDeviceFacts(this.facts),
      settings: this.settings,
    };
  }

  _setState(state, text) {
    this.sessionState = state;
    this.stateText = text;
  }

  // --- 端口与手柄 ------------------------------------------------

  async refreshPorts() {
    this.ports = await this.listPorts();
    this.port = portSelection(this.ports, this.port, this.args.port, this.portChosen);
    this.log(portSummary(this.ports, this.port));
  }

  async refreshPads() {
    if (this.hid === null) {
      try {
        this.hid = this.loadHidFn();
        this.hidProblem = '';
      } catch (exc) {
        if (!(exc instanceof HidUnavailable)) {
          throw exc;
        }
        this.hidProblem = exc.message;
        this.logError(this.hidProblem);
        this.log('手柄转发相关控件不可用，串口命令与截图照常');
        return;
      }
    }
    let candidates;
    try {
      candidates = listCandidates(this.hid.enumerate());
    } catch (exc) {
      this.logError(`枚举手柄失败：${exc.message}`);
      return;
    }
    this.padEntries = candidates;
    if (this.padIndex !== null && this.padIndex >= candidates.length) {
      this.padIndex = null;
    }
    this.log(`候选手柄 ${candidates.length} 个，自动接入选中的那只`);
  }

  logPadCandidates() {
    if (this.hid === null) {
      this.logError(this.hidProblem || 'hidapi 不可用');
      return;
    }
    let candidates;
    try {
      candidates = listCandidates(this.hid.enumerate());
    } catch (exc) {
      this.logError(`枚举手柄失败：${exc.message}`);
      return;
    }
    if (!candidates.length) {
      this.log('没有找到手柄接口');
      return;
    }
    for (const info of candidates) {
      this.log(describe(info));
    }
  }

  selectPad(index) {
    const target = Number.isInteger(index) && index >= 0 && index < this.padEntries.length ? index : null;
    this.padIndex = target;
    const name = target === null ? '自动（固件/工具挑第一只）' : describe(this.padEntries[target]);
    this.log(`输入手柄已选中：${name}`);
    if (this.session !== null && this.session.padInfo !== null) {
      this.log('已接入的手柄会在下一轮重新接入新选中的那只');
    }
  }

  selectedPadPath() {
    return this.padIndex === null ? null : this.padEntries[this.padIndex].path;
  }

  // --- 连接与断开 ------------------------------------------------

  async connectSerial(port = null) {
    if (this.connected) {
      this.logError('已有会话在跑，先断开再连');
      return;
    }
    const target = (port ?? this.port ?? '').trim();
    if (!target) {
      this.logError('先选一个串口再连接');
      return;
    }
    this._setState('connecting', `正在打开 ${target}`);
    let conn;
    try {
      conn = this.createSerial(target, this.args.baud);
    } catch (exc) {
      const hint = openHint(exc);
      this.logError(`${target}: ${hint}`);
      this._setState('disconnected', '未连接');
      return;
    }
    this._startSession(conn, target);
  }

  async connectNet(target) {
    if (this.connected) {
      this.logError('已有会话在跑，先断开再连');
      return;
    }
    const text = (target ?? '').trim();
    if (!text) {
      this.logError('先填 设备IP:端口 再网络连接（串口连接不需要它）');
      return;
    }
    let host;
    let port;
    try {
      [host, port] = parseEndpoint(text);
    } catch (exc) {
      this.logError(exc.message);
      return;
    }
    this._setState('connecting', `正在连接 ${host}:${port}（UDP）`);
    let conn;
    try {
      conn = this.createUdp(host, port);
    } catch (exc) {
      this.logError(exc.message);
      this._setState('disconnected', '未连接');
      return;
    }
    this._startSession(conn, `${host}:${port}（网络）`);
  }

  _startSession(conn, description) {
    this.args.padPath = this.selectedPadPath();
    const session = new this.Session(this.args, this.hid, conn, this.makeReporter());
    for (const command of SETTINGS_READ_COMMANDS) {
      session.commands.push(command);
    }
    // UdpLink 以 address 标识网络会话（截图只走串口就靠这一位）。
    this.netSession = conn.address ?? null;
    if (this.netSession === null) {
      this.args.port = description;
      this.portChosen = true;
    }
    this.port = description;
    this.session = session;
    this.link = conn;
    this._setState('connected', `已连接 ${description}`);
    const mode = this.netSession !== null ? '网络，截图仍需串口' : '串口';
    this.log(`会话已建立：${description} ${mode}，转发默认开启`);
    session.runInteractive(false)
      .catch((exc) => {
        this.logError(`会话异常结束：${exc.message ?? exc}`);
      })
      .finally(() => {
        try {
          session.detachPad();
        } catch {
          // 链路已经死了就不再补发 DETACH。
        }
        conn.close();
        this.makeReporter().event('session_closed', { code: session.stopCode });
      });
  }

  disconnect() {
    const session = this.session;
    if (session === null) {
      return;
    }
    this._setState('connecting', '正在断开');
    session.stop = true;
  }

  /**  Reporter 面：行进日志（先吸收回读），错误更新 lastError，事件进状态机。 */
  makeReporter() {
    return {
      line: (text) => {
        this.absorbReply(text);
        this.log(text);
      },
      error: (text) => this.logError(text),
      event: (name, fields = {}) => {
        this.handleEvent(name, fields);
        // 状态性事件由 /api/state 表达，日志面板只补人话行。
        if (name === 'shot_saved') {
          this.log(`截图已落盘：${fields.path}`);
        } else if (name === 'pad_attached') {
          this.log(`手柄已接入：${fields.describe}`);
        } else if (name === 'pad_detached') {
          this.log('手柄已断开（已向设备发 DETACH）');
        }
      },
    };
  }

  /** 升级后的等待：与命令行 --wait 同一实现，设备回来后自动重连串口。 */
  async waitDeviceBack() {
    this.log('等设备重启回来（最多 60 秒）……');
    const code = await this.waitForVersion(this.args.port, this.args.baud, this.makeReporter());
    if (code === 0) {
      this.handleEvent('device_back', {});
    } else {
      this.logError('设备没有在期限内回到串口，请手动重连');
    }
  }

  // --- 事件与回读吸收 --------------------------------------------

  handleEvent(name, fields) {
    switch (name) {
    case 'shot_saved':
      try {
        this.openPath(fields.path);
      } catch (exc) {
        this.logError(`打开截图失败：${exc.message}`);
      }
      break;
    case 'ota_progress': {
      const total = Math.max(Number(fields.total) || 1, 1);
      const confirmed = Number(fields.confirmed) || 0;
      this.otaPercent = Math.min(confirmed / total, 1);
      this.otaLabel = `写入 ${confirmed} / ${fields.total} 字节（${Math.floor((confirmed * 100) / total)}%）`;
      break;
    }
    case 'ota_finished': {
      const ok = Boolean(fields.ok);
      // 进度只在写入期间表达；完成与失败都归零，结果由文字说明。
      this.otaPercent = 0;
      this.otaLabel = ok ? '升级完成' : '升级失败';
      // 网络升级后设备重启、netlog 会话随之关闭，「等设备回来」只对串口成立。
      if (this.netSession !== null && ok) {
        this.pendingOtaWait = false;
        this.log('设备重启后 netlog 会话已关：在设备「无线调试」页重开会话，'
                        + '再点「网络连接」；在跑版本用命令页的 version 确认');
        return;
      }
      this.pendingOtaWait = ok && this.otaWait;
      if (ok && !this.pendingOtaWait) {
        this.log('设备重启后点「串口连接」重新连上');
      }
      break;
    }
    case 'session_closed': {
      const code = Number(fields.code ?? 1);
      this.session = null;
      this.link = null;
      this.netSession = null;
      this._setState(code === 0 ? 'disconnected' : 'broken',
        code === 0 ? '会话已结束' : '链路断开');
      if (this.pendingOtaWait) {
        this.pendingOtaWait = false;
        this.waitDeviceBack();
      } else if (code !== 0) {
        this.logError('链路已断开：检查设备是否重启或拔线，然后重新连接');
      }
      break;
    }
    case 'device_back':
      this.log('设备已回到串口，正在重新连接');
      this.connectSerial();
      break;
    case 'link_error':
      this.lastError = fields.message ?? '';
      break;
    default:
      break;
    }
  }

  /** 设备回读行 → 展示态：固件是唯一事实源，这里只跟着回读走。 */
  absorbReply(text) {
    const parsed = parseDeviceReply(text);
    if (parsed === null) {
      return;
    }
    const { channel, fields } = parsed;
    if ('light' in fields) {
      this.settings.brightness = fields.light;
    }
    if ('screen_on' in fields) {
      this.settings.screenOn = fields.screen_on;
    }
    if (channel === 'ctrl') {
      this.settings.colors = [fields.body, fields.button, fields.accent, fields.grip];
      const hex = (value) => `0x${value.toString(16).padStart(6, '0')}`;
      this.settings.colorSummary = '当前：' + this.settings.colors.map(hex).join(' ');
    } else if (channel === 'ds') {
      this.settings.dsTouchpad = fields.touchpad_plus_minus;
      this.settings.dsCapture = fields.capture_key;
    } else if (channel === 'device') {
      this.facts = { ...this.facts, ...fields };
      if ('light' in fields) {
        this.settings.brightness = fields.light;
      }
      if ('screen_on' in fields) {
        this.settings.screenOn = fields.screen_on;
      }
    } else if (channel === 'netlog_cred') {
      this.settings.wifiSsid = fields.ssid;
      this.settings.wifiPass = fields.pass;
    } else if (channel === 'netlog') {
      this.settings.netSummary = `设备：${fields.state} · ${fields.ssid} · ${fields.dest}`;
    }
  }

  // --- 命令与设置动作 --------------------------------------------

  sendCommand(text) {
    const command = text.trim();
    if (!command) {
      return;
    }
    // 截图要完整字节流（图像帧无重传），UDP 上不做；OTA 有窗口重发，网络会话照常。
    if (this.netSession !== null && command.split(/\s+/)[0] === ':shot') {
      this.logError('截图只走串口（图像帧没有重传）：请用 COM 口连接后再试');
      return;
    }
    if (this.session === null) {
      this.logError('未连接设备，命令没有发出');
      return;
    }
    this.session.commands.push(command);
    // 回显带「> 」前缀：脱敏的 SAVE/CONNECT 模式靠它区分命令与固件日志词。
    this.emit({ kind: 'send', text: `> ${command}` });
  }

  setForward(enabled) {
    const session = this.session;
    if (session === null) {
      this.log('连接后转发开关才会生效（默认开启）');
      return;
    }
    session.forward = enabled;
    this.log(enabled ? '手柄转发已开启' : '手柄转发已关闭（设备会收到 DETACH）');
    if (!enabled) {
      this.log('注意：用户自己按的组合键或串口 ui 命令不受影响');
    }
  }

  saveWifi(ssid, password) {
    const name = (ssid ?? '').trim();
    const pass = password ?? '';
    if (!name || !pass) {
      this.logError('SSID 与密码都要填');
      return;
    }
    if (name.includes(' ') || pass.includes(' ')) {
      this.logError('SSID 与密码暂不支持空格');
      return;
    }
    this.sendCommand(`netlog save ${name} ${pass}`);
  }

  // --- 升级 ------------------------------------------------------

  validateImage(imagePath) {
    const target = (imagePath ?? '').trim();
    try {
      const { data, version } = loadImage(target);
      this.log(`镜像校验通过：${target}（${data.length} 字节，版本 ${version}）`);
      return { ok: true, bytes: data.length, version };
    } catch (exc) {
      if (!(exc instanceof ImageError)) {
        throw exc;
      }
      this.logError(exc.message);
      this.otaLabel = '镜像不合法';
      return { ok: false, error: exc.message };
    }
  }

  startUpgrade(imagePath, wait) {
    if (this.session === null) {
      this.logError('先连接设备再升级');
      return { ok: false, error: '先连接设备再升级' };
    }
    const check = this.validateImage(imagePath);
    if (!check.ok) {
      return check;
    }
    this.pendingOtaWait = false;
    this.otaWait = Boolean(wait);
    this.otaPercent = 0;
    this.otaLabel = '准备写入……';
    this.sendCommand(`:ota ${(imagePath ?? '').trim()}`);
    return { ok: true };
  }

  shutdown() {
    if (this.session === null) {
      return;
    }
    this.session.stop = true;
  }
}

/** REST + 静态文件请求处理：API 挂 /api/*，其余落到前端构建目录。 */
export function createRequestHandler(gui, distDir) {
  async function handleApi(request, response, url) {
    const action = url.pathname.slice('/api/'.length);
    const body = await readJsonBody(request);
    let payload;
    if (request.method === 'GET' && action === 'state') {
      payload = gui.snapshot();
    } else if (request.method === 'POST' && action === 'connect-serial') {
      await gui.connectSerial(body.port ?? null);
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'connect-net') {
      await gui.connectNet(body.net ?? '');
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'disconnect') {
      gui.disconnect();
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'ports-refresh') {
      await gui.refreshPorts();
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'pads-refresh') {
      await gui.refreshPads();
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'pad-candidates') {
      gui.logPadCandidates();
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'pad-select') {
      gui.selectPad(body.index ?? null);
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'command') {
      gui.sendCommand(String(body.text ?? ''));
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'forward') {
      gui.setForward(Boolean(body.enabled));
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'wifi-save') {
      gui.saveWifi(body.ssid, body.pass);
      payload = { ok: true };
    } else if (request.method === 'POST' && action === 'ota-validate') {
      payload = gui.validateImage(body.path);
    } else if (request.method === 'POST' && action === 'ota-start') {
      payload = gui.startUpgrade(body.path, body.wait);
    } else {
      response.writeHead(404, { 'content-type': 'application/json; charset=utf-8' });
      response.end(JSON.stringify({ error: `未知 API：${request.method} ${url.pathname}` }));
      return;
    }
    response.writeHead(200, { 'content-type': 'application/json; charset=utf-8' });
    response.end(JSON.stringify(payload));
  }

  return function handler(request, response) {
    const url = new URL(request.url, 'http://localhost');
    if (url.pathname.startsWith('/api/')) {
      handleApi(request, response, url).catch((exc) => {
        response.writeHead(500, { 'content-type': 'application/json; charset=utf-8' });
        response.end(JSON.stringify({ error: exc.message ?? String(exc) }));
      });
      return;
    }
    serveStatic(distDir, url.pathname, response);
  };
}

async function readJsonBody(request) {
  if (request.method !== 'POST') {
    return {};
  }
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > 1024 * 1024) {
      throw new Error('请求体超过 1MB');
    }
    chunks.push(chunk);
  }
  const text = Buffer.concat(chunks).toString('utf8').trim();
  return text ? JSON.parse(text) : {};
}

function serveStatic(distDir, pathname, response) {
  const relative = pathname === '/' ? 'index.html' : decodeURIComponent(pathname.slice(1));
  const filePath = path.join(distDir, relative);
  if (!filePath.startsWith(path.resolve(distDir))) {
    response.writeHead(403);
    response.end('forbidden');
    return;
  }
  fs.readFile(filePath, (exc, data) => {
    if (exc) {
      const hint = 'GUI 前端尚未构建：在 pc/gui 下执行 pnpm install 与 pnpm build'
                + '（开发调试用 pnpm dev，Vite 会代理 /api 与 /ws）。';
      response.writeHead(503, { 'content-type': 'text/plain; charset=utf-8' });
      response.end(hint);
      return;
    }
    const type = MIME_TYPES[path.extname(filePath).toLowerCase()] ?? 'application/octet-stream';
    // index.html 不缓存：前端迭代期防止旧 HTML 配新哈希产物的混搭。
    const cacheControl = relative === 'index.html' ? 'no-cache' : 'public, max-age=3600';
    response.writeHead(200, { 'content-type': type, 'cache-control': cacheControl });
    response.end(data);
  });
}

/** 仓库内前端构建产物目录（pc/gui/dist）。 */
export function defaultDistDir() {
  return path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..', 'gui', 'dist');
}
