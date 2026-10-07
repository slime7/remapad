// 桥接 + 命令行会话：串口只有这一个持有者。转发、截图、升级、amiibo 与采集
// 都挂在这一条主循环上；图形界面传 readStdin=false，命令由界面塞进队列。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { createInterface as createReadline } from 'node:readline';

import {
  CONN_BT,
  CONN_USB,
  FrameDecoder,
  TYPE_AMIIBO_ACK,
  TYPE_DETACH,
  TYPE_ATTACH,
  TYPE_FEEDBACK,
  TYPE_HOST_RAW,
  TYPE_IMAGE_DATA,
  TYPE_IMAGE_END,
  TYPE_IMAGE_INFO,
  TYPE_OTA_ACK,
  TYPE_OUT_REPORT,
  TYPE_PING,
  TYPE_REPORT,
  encode,
  feedbackParams,
  parseAmiiboAck,
  parseHostRaw,
  parseOtaAck,
} from '../link/frame.js';
import { UdpLink } from '../link/net.js';
import { SerialLink } from '../link/serial.js';
import { btHapticsWanted } from '../args.js';
import { Bt36OpusEncoder, Ds5HapticsAudio, Ds5HapticsBt } from '../haptics/index.js';
import { describe, identityPayload, pickDevice, connFor } from '../input/pads.js';
import { AmiiboJob } from './amiibo-job.js';
import { HostCaptureSink } from './capture.js';
import { ConsoleReporter } from './reporter.js';
import { FeedbackThrottle, WriteBackGate } from './gates.js';
import { AmiiboError, loadAmiibo, loadImage } from './image.js';
import { OtaJob } from './ota.js';
import { ShotCollector, defaultShotPath, writePng } from './shot.js';
import { now, sleep } from '../util.js';

/** 一键拉取设备数据的命令清单（每个都有无参回读；见 firmware/main/console/cli.c）。 */
export const ALL_QUERIES = [
  'status', 'mem', 'version', 'link', 'pad', 'usb', 'report', 'ui', 'adv', 'headset',
  'fwver', 'fwack', 'fwpost', 'fwapply', 'ctrl', 'backlight', 'screen', 'relay',
  'motion', 'ltk', 'rumble', 'lamp', 'haptic',
];

/** 升级后等设备回来的超时与轮询间隔。 */
export const REOPEN_TIMEOUT_S = 60.0;
export const REOPEN_INTERVAL_S = 1.0;

export class Session {
  /** @param args parseCtrlArgs 的产物（或字段兼容的普通对象）
     * @param hid loadHid 的句柄（enumerate/open），--no-pad 时为 null
     * @param conn SerialLink 或 UdpLink */
  constructor(args, hid, conn, reporter = null) {
    this.args = args;
    this.hid = hid;
    this.link = conn;
    this.reporter = reporter ?? new ConsoleReporter();
    this.decoder = new FrameDecoder();
    this.pad = null;
    this.padInfo = null;
    this.ident = Buffer.alloc(0);
    this.seq = 0;
    this.reports = 0;
    this.frames = 0;
    this.outputs = 0;
    this.nextSend = 0.0;
    this.nextRescan = 0.0;
    this.interval = args.maxRate > 0 ? 1.0 / args.maxRate : 0.0;
    this.lastStat = now();
    this.lineBuf = '';
    this.printLogs = Boolean(args.logs || args.verbose);
    this.interactive = false;
    this.logMode = false;
    this.logRaw = false;
    this.logStarted = 0.0;
    this.logDeadline = 0.0;
    this.oneShot = false;
    this.expectReply = false;
    this.replySeen = false;
    this.replyDeadline = 0.0;
    this.replyHardDeadline = 0.0;
    // 是否把手柄转发给设备：交互模式默认转发，一次性命令 / 截图 / 日志 /
    // 升级默认不碰手柄（否则主机会看到手柄闪一下），--pad 可显式打开。
    this.forward = false;
    this.commands = [];
    this.stdin = null;
    this.shot = new ShotCollector();
    this.shotPath = null;
    this.shotReady = false;
    this.ota = null;
    this.amiibo = null;
    /** 主机原始输出采集（:capture / --capture 开启）：落盘器挂在会话上。 */
    this.capture = null;
    this.captured = 0;
    this.stop = false;
    this.stopCode = 0;
    /** 0x31 写回的短写计数（返回值 < 报告长度 = 驱动没收下）。 */
    this.writebackShort = 0;
    // 反馈帧打印限频（不影响写回手柄）与写回手柄的限速门。
    this.feedbackGate = new FeedbackThrottle();
    this.writeGate = new WriteBackGate();
    // DS5 桥接时的 PC 侧音频触觉（attach 时按需启动）。
    this.haptics = null;
    this._hapticsStarting = false;
    // 音频流开好后待发的「haptic audio on」：串口只允许主循环一个写者。
    this._hapticsNotify = false;
    // 触觉流写回失败：主循环收尾并回落 HID 震动写回。
    this._hapticsLost = false;
    this._attaching = false;
    /** 主循环每拍的开场钩子（MCP 引擎挂命令下发用）；null = 无。 */
    this.pumpHook = null;
  }

  // --- 手柄转发 --------------------------------------------------

  async attachPad(info) {
    const device = await this.hid.open(info.path);
    this.pad = device;
    this.padInfo = info;
    this.ident = identityPayload(info, 0, 0);
    this.link.write(encode(TYPE_ATTACH, 0, this.seq, this.ident));
    const description = describe(info);
    this.reporter.line(`设备接入：${description}`);
    this.reporter.event('pad_attached', { describe: description });
    this.maybeStartHaptics();
  }

  /** DS5 接入时启用 PC 侧音频触觉：USB 直插走 4ch 音频端点（频道 3/4 触觉、
     * 1/2 发声）。蓝牙接入默认启用私有触觉流（0x36 HD 触觉 + 手柄喇叭真声）——
     * DS5 的 HD 触觉与手柄喇叭只有这条流承载；--no-bt-haptics 关掉回落 HID 写回。
     * --no-audio-haptics / --no-rumble 或通路开不起来时静默回落 HID 震动。
     * 开流不能占着桥接热路径，启动放异步。 */
  maybeStartHaptics() {
    if (this.haptics !== null || this._hapticsStarting) {
      return;
    }
    if (this.args.noAudioHaptics || this.args.noRumble) {
      return;
    }
    const info = this.padInfo ?? {};
    if (info.vendorId !== 0x054c || ![0x0ce6, 0x0df2].includes(info.productId)) {
      return;
    }
    if (connFor(info) === CONN_BT) {
      if (!btHapticsWanted(this.args)) {
        this.reporter.line('蓝牙接入：--no-bt-haptics 保持 HID 震动写回（HD 触觉与手柄喇叭不启用）');
        return;
      }
      this._hapticsStarting = true;
      this._startBtHaptics();
      return;
    }
    if (connFor(info) !== CONN_USB) {
      this._hapticsStarting = false;
      return;
    }
    this._hapticsStarting = true;
    this._startAudioHaptics();
  }

  async _startAudioHaptics() {
    const audio = new Ds5HapticsAudio(this.reporter);
    if (!(await audio.start())) {
      this._hapticsStarting = false;
      return;
    }
    this._adoptHaptics(audio);
  }

  async _startBtHaptics() {
    // 0x32 音圈流兜底；Opus 可用时升级 0x36（HD 触觉 + 手柄喇叭真声），
    // 编码器建不起来就按 0x32 走。
    let encoder = null;
    try {
      encoder = new Bt36OpusEncoder();
    } catch {
      encoder = null;
    }
    const audio = new Ds5HapticsBt(this.pad, this.reporter, {
      onError: () => this._loseHaptics(),
      speakerEncoder: encoder,
    });
    if (!(await audio.start())) {
      this._hapticsStarting = false;
      return;
    }
    this._adoptHaptics(audio);
  }

  /** 触觉流写回失败：主循环收尾回落 HID 震动。 */
  _loseHaptics() {
    this._hapticsLost = true;
  }

  _adoptHaptics(audio) {
    if (this.pad === null) {
      // 启动期间手柄已断开（会话收尾）：不留孤儿流。
      audio.stop();
      this._hapticsStarting = false;
      return;
    }
    this.haptics = audio;
    this._hapticsStarting = false;
    // 「haptic audio on」由主循环发：让位要等私有流真的接到 HD 子帧。
    this._hapticsNotify = false;
  }

  pumpHapticsNotify() {
    if (this._hapticsLost) {
      this._hapticsLost = false;
      if (this.haptics !== null) {
        this.reporter.error('触觉流写回被拒，回落 HID 震动写回');
        this.stopHaptics();
      }
    }
    if (this.haptics === null || this._hapticsNotify) {
      return;
    }
    // 让位（haptic audio on）等私有流真的接到 HD 子帧之后再发：没接到内容
    // 的通路不驱动音圈（老固件、布局行没声明 HD 通路），提前让位会把手柄
    // 留在「HID 震动已清零、音频也没有内容」的静默状态。
    if (!this.haptics.engaged) {
      return;
    }
    this._hapticsNotify = true;
    try {
      this.sendCli('haptic audio on');
      this.reporter.line(this.haptics.label ?? Ds5HapticsAudio.LABEL);
      this.reporter.event('haptics_audio', { state: 'on' });
    } catch {
      // 链路写不进去就收掉触觉流，回落 HID 震动写回。
      this.stopHaptics();
    }
  }

  stopHaptics() {
    if (this.haptics === null) {
      return;
    }
    // 蓝牙触觉流的 stats 是方法，先在实例上算好文本再停，避免解绑调用丢 this。
    const stats = typeof this.haptics.stats === 'function' ? this.haptics.stats() : null;
    this.haptics.stop();
    if (stats !== null) {
      this.reporter.line(stats);
    }
    this.haptics = null;
    try {
      this.sendCli('haptic audio off');
      this.reporter.event('haptics_audio', { state: 'off' });
    } catch {
      // 链路已在收尾时静默。
    }
  }

  async detachPad() {
    this.stopHaptics();
    if (this.pad === null) {
      return;
    }
    try {
      this.link.write(encode(TYPE_DETACH, 0, this.seq, this.ident));
    } catch {
      // 链路已坏时尽力而为。
    }
    try {
      await this.pad.close();
    } catch {
      // 关闭失败不阻塞收尾。
    }
    this.pad = null;
    this.padInfo = null;
    this.reporter.event('pad_detached', {});
  }

  async pumpPad(nowSeconds) {
    if (this.hid === null || !this.forward) {
      return;
    }
    if (this.pad === null) {
      if (this._attaching) {
        return;
      }
      if (nowSeconds < this.nextRescan) {
        return;
      }
      this.nextRescan = nowSeconds + 0.5;
      const info = pickDevice(this.args, this.hid.enumerate());
      if (info === null) {
        return;
      }
      this._attaching = true;
      this.attachPad(info)
        .catch((exc) => {
          this.reporter.error(`手柄接入失败：${exc.message}`);
        })
        .finally(() => {
          this._attaching = false;
        });
      return;
    }
    if (this.args.padPath !== null && this.padInfo.path !== this.args.padPath) {
      // 图形界面里换了手柄：在本任务内断开，下一轮重新接入选中的那只。
      await this.detachPad();
      return;
    }
    const data = await this.pad.read();
    if (data && data.length && nowSeconds >= this.nextSend) {
      const raw = Buffer.from(data);
      const payload = Buffer.concat([
        identityPayload(this.padInfo, raw.length ? raw[0] : 0, raw.length),
        raw,
      ]);
      this.link.write(encode(TYPE_REPORT, 0, this.seq, payload));
      this.seq = (this.seq + 1) & 0xff;
      this.reports += 1;
      this.nextSend = nowSeconds + this.interval;
    }
  }

  /** 把设备编码好的输出报告写回手柄：布局知识只在固件里有一份。
     * 写回经 WriteBackGate 限速——蓝牙 HID 写回慢；被挡下的帧由 pump 在窗口
     * 到期后放行最新一帧。 */
  writeOutputReport(payload) {
    if (this.args.noRumble || this.pad === null || !payload.length) {
      return Promise.resolve(false);
    }
    if (!this.writeGate.admit(payload, now())) {
      return Promise.resolve(false);
    }
    return this.sendOutputReport(payload);
  }

  async sendOutputReport(payload) {
    let written;
    try {
      written = await this.pad.write(payload);
    } catch (exc) {
      this.reporter.error(`反馈写回失败：${exc.message}`);
      return false;
    }
    // node-hid 在 Windows 会把短于描述符声明长度的写回补齐到声明长度再交驱动
    // （DS5 蓝牙集合声明 547，0x31 的 78 字节写法因此返回 547）——返回值比
    // 载荷长是常态，只有真的少交（< 载荷长度）才是这一份没写进去。
    if (typeof written === 'number' && written < payload.length) {
      this.writebackShort += 1;
      if (this.writebackShort === 1 || this.writebackShort % 200 === 0) {
        this.reporter.error(`反馈写回短写：${written}/${payload.length} 字节（累计 ${this.writebackShort} 份）`);
      }
      return false;
    }
    return true;
  }

  // --- 链路读取 --------------------------------------------------

  async pumpLink(nowSeconds) {
    const chunk = this.link.read();
    if (!chunk || !chunk.length) {
      return;
    }
    const { frames, text } = this.decoder.feed(chunk);
    for (const { type, slot, payload } of frames) {
      this.frames += 1;
      if (type === TYPE_FEEDBACK) {
        const params = feedbackParams(payload);
        if (params !== null && this.haptics !== null) {
          this.haptics.setParams(params);
        }
        const line = this.feedbackGate.feed(payload, nowSeconds);
        if (line !== null) {
          this.reporter.line(line);
        }
      } else if (type === TYPE_OUT_REPORT) {
        if (await this.writeOutputReport(payload)) {
          this.outputs += 1;
        }
      } else if (type === TYPE_HOST_RAW) {
        // 主机输出的原始采集：有落盘器写文件，没有就只计数。
        this.captured += 1;
        if (this.capture !== null) {
          this.capture.writeRecord(parseHostRaw(payload), slot, nowSeconds);
        }
      } else if (type === TYPE_PING) {
        this.reporter.line(`设备在线（协议 v${payload.length ? payload[0] : 0}）`);
      } else if (type === TYPE_IMAGE_INFO) {
        this.shot.onInfo(payload);
      } else if (type === TYPE_IMAGE_DATA) {
        this.shot.onData(payload);
      } else if (type === TYPE_IMAGE_END) {
        this.finishShot(payload);
      } else if (type === TYPE_OTA_ACK && this.ota !== null) {
        this.ota.onAck(parseOtaAck(payload));
      } else if (type === TYPE_AMIIBO_ACK && this.amiibo !== null) {
        this.amiibo.onAck(parseAmiiboAck(payload));
      }
    }
    if (text.length) {
      this.handleText(text);
    }
  }

  handleText(text) {
    this.lineBuf += text.toString('utf8');
    for (;;) {
      const index = this.lineBuf.indexOf('\n');
      if (index < 0) {
        break;
      }
      const line = this.lineBuf.slice(0, index);
      this.lineBuf = this.lineBuf.slice(index + 1);
      this.handleLine(line.replace(/\r$/, ''));
    }
  }

  handleLine(line) {
    if (!line) {
      return;
    }
    if (this.logMode && !this.logRaw && this.logStarted) {
      this.reporter.line(`[${(now() - this.logStarted).toFixed(2).padStart(7)}s] ${line}`);
    } else {
      this.reporter.line(line);
    }
    if (this.expectReply) {
      // 任何一行都算「设备还在说话」：status / link / pad 这类回复不以 ok 开头，
      // 固件日志也会夹在中间；每条新行把静默窗往后推，硬截止兜住总时长。
      const quiet = line.startsWith('ok') || line.startsWith('err') || line.startsWith('pong') ? 0.15 : 0.25;
      this.replySeen = true;
      const hard = this.replyHardDeadline || Number.MAX_VALUE;
      this.replyDeadline = Math.min(hard, now() + quiet);
    }
  }

  // --- 命令行 ----------------------------------------------------

  sendCli(command) {
    this.link.write(Buffer.from(`${command}\r`, 'utf8'));
    this.link.flush();
  }

  startStdin() {
    // 交互模式的命令行来源：stdin 按行进队列，主循环统一发。
    const rl = createReadline({ input: process.stdin, terminal: false });
    rl.on('line', (line) => {
      if (this.stop) {
        return;
      }
      this.commands.push(line.replace(/[\r\n]+$/, ''));
    });
    rl.on('close', () => {
      if (this.stdin === rl) {
        this.stdin = null;
      }
    });
    this.stdin = rl;
  }

  pumpCommands() {
    for (;;) {
      const line = this.commands.shift();
      if (line === undefined) {
        return;
      }
      const trimmed = line.trim();
      if (!trimmed) {
        continue;
      }
      if (trimmed.startsWith(':')) {
        this.runLocal(trimmed.slice(1));
      } else {
        this.sendCli(trimmed);
      }
    }
  }

  runLocal(text) {
    const splitAt = text.indexOf(' ');
    const name = splitAt < 0 ? text : text.slice(0, splitAt);
    const argumentsText = splitAt < 0 ? '' : text.slice(splitAt + 1).trim();
    const parts = argumentsText.split(/\s+/).filter(Boolean);
    if (['help', 'h', '?'].includes(name)) {
      printLocalHelp(this.reporter);
    } else if (name === 'all') {
      this.runAll();
    } else if (name === 'shot') {
      // 截图路径整段当参数：路径里有空格也不用引号（图形界面的截图按钮同样走这里）。
      this.requestShot(argumentsText || null);
    } else if (name === 'log') {
      if (parts.length && parts[0] === 'off') {
        this.logMode = false;
        this.reporter.line('日志透传关闭');
        return;
      }
      const seconds = parts.length ? Number(parts[0]) : 15.0;
      this.logMode = true;
      this.logStarted = now();
      this.logDeadline = seconds > 0 ? this.logStarted + seconds : 0.0;
      this.reporter.line(`透传设备日志 ${seconds.toFixed(0)} 秒（0 表示持续到 :log off）`);
    } else if (name === 'ota') {
      this.requestUpgrade(argumentsText || null);
    } else if (name === 'amiibo') {
      if (!argumentsText) {
        this.reporter.error('用法：:amiibo <bin 文件路径>（540 字节 NTAG215 dump）');
        return;
      }
      try {
        this.requestAmiiboUpload(argumentsText);
      } catch (exc) {
        if (exc instanceof AmiiboError) {
          this.reporter.error(exc.message);
        } else {
          throw exc;
        }
      }
    } else if (name === 'capture') {
      this.runCaptureCommand(argumentsText);
    } else if (['quit', 'q', 'exit'].includes(name)) {
      this.stop = true;
    } else {
      this.reporter.error(`未知的工具命令：${name}（:help 看清单）`);
    }
  }

  runCaptureCommand(argumentsText) {
    /** :capture <路径> 开始落盘、:capture off 停止、无参看状态。 */
    if (argumentsText === 'off') {
      if (this.capture === null) {
        this.reporter.line('主机原始输出采集未开启');
      } else {
        this.stopCapture();
      }
      return;
    }
    if (!argumentsText) {
      if (this.capture === null) {
        this.reporter.line('主机原始输出采集未开启（:capture <文件路径> 开始，'
                    + '文件存主机写进手柄/设备之前的原始字节）');
      } else {
        this.reporter.line(`采集中 → ${this.capture.path}（已落盘 `
                    + `${this.capture.count} 条；:capture off 停止）`);
      }
      return;
    }
    if (this.capture !== null) {
      this.stopCapture();
    }
    this.startCapture(argumentsText);
  }

  /** 开一个采集落盘器并让固件开始上行（设备命令 capture on）。 */
  startCapture(pathStr) {
    let sink;
    try {
      sink = new HostCaptureSink(expandUser(pathStr), now());
      sink.open();
    } catch (exc) {
      this.reporter.error(`打不开采集文件：${exc.message}`);
      return;
    }
    this.capture = sink;
    try {
      this.sendCli('capture on');
    } catch (exc) {
      this.capture = null;
      sink.close();
      throw exc;
    }
    this.reporter.line(`主机原始输出采集已开启 → ${sink.path}`
            + '（震动/玩家灯/指令等主机输出的原始字节，:capture off 停止）');
    this.reporter.event('capture_started', { path: sink.path });
  }

  stopCapture() {
    if (this.capture === null) {
      return;
    }
    const sink = this.capture;
    this.capture = null;
    try {
      this.sendCli('capture off');
    } catch {
      // 链路已坏时只落盘收尾。
    }
    const summary = sink.close();
    if (summary) {
      this.reporter.line(summary);
    }
    this.reporter.event('capture_stopped', { count: sink.count });
  }

  requestShot(path) {
    this.shotPath = path ? expandUser(path) : defaultShotPath();
    this.shotReady = false;
    this.shot.begin(now(), this.args.shotTimeout);
    this.sendCli('shot');
  }

  finishShot(payload) {
    if (this.shot.buffer === null) {
      return;
    }
    const problem = this.shot.onEnd(payload);
    if (problem) {
      this.reporter.error(problem);
      return;
    }
    // 交互模式里设备命令 shot 是用户直接敲的，走到这里才决定落盘路径。
    const path = this.shotPath ?? defaultShotPath();
    writePng(path, this.shot.width, this.shot.height, this.shot.buffer);
    this.shotPath = path;
    this.reporter.line(`截图已保存：${path}（${this.shot.width}x${this.shot.height}，`
            + `${this.shot.chunks} 块）`);
    this.reporter.event('shot_saved', {
      path, width: this.shot.width, height: this.shot.height, chunks: this.shot.chunks,
    });
    this.shotReady = true;
  }

  requestUpgrade(imagePath) {
    const path = imagePath ? expandUser(imagePath) : this.args.image;
    const { data: image, version } = loadImage(path);
    this.reporter.line(`镜像 ${path}：${image.length} 字节，版本 ${version}`);
    this.reporter.event('ota_started', { path, size: image.length, version });
    const lossy = this.link instanceof UdpLink;
    if (lossy) {
      this.reporter.line('链路是 WiFi（UDP）：丢包靠窗口重发兜住，速度比串口慢；'
                + '设备重启后 netlog 会话要重新打开');
    }
    this.ota = new OtaJob(image, version, (data) => this.link.write(data), this.reporter, lossy);
    this.ota.start(now());
  }

  requestAmiiboUpload(amiiboPath) {
    const path = expandUser(amiiboPath);
    const { name, data } = loadAmiibo(path);
    this.amiibo = new AmiiboJob(name, data, (payload) => this.link.write(payload), this.reporter);
    this.amiibo.start(now());
  }

  // --- 主循环 ----------------------------------------------------

  async pump(nowSeconds) {
    if (this.pumpHook !== null) {
      this.pumpHook(nowSeconds);
    }
    await this.pumpPad(nowSeconds);
    await this.pumpLink(nowSeconds);
    this.pumpCommands();
    this.pumpHapticsNotify();
    // 写回限速门的待写帧放行：窗口到期后把最新一帧补写出去，
    // 停震的收尾帧不因限速丢失。
    const pending = this.writeGate.poll(nowSeconds);
    if (pending !== null && this.pad !== null && !this.args.noRumble) {
      if (await this.sendOutputReport(pending)) {
        this.outputs += 1;
      }
    }
    if (this.ota !== null) {
      this.ota.tick(nowSeconds);
      if (this.ota.finished) {
        this.stop = true;
        this.stopCode = this.ota.exitCode;
      }
    }
    if (this.amiibo !== null) {
      this.amiibo.tick(nowSeconds);
      if (this.amiibo.finished) {
        const code = this.amiibo.exitCode;
        this.amiibo = null;
        // 交互模式的 :amiibo 不退出会话（传完可以接着 select / list）；
        // 一次性 --amiibo 在这里收口退出。
        if (!this.interactive) {
          this.stop = true;
          this.stopCode = code;
        }
      }
    }
    if (this.logMode && this.logDeadline && nowSeconds >= this.logDeadline) {
      this.logMode = false;
      this.logDeadline = 0.0;
      this.reporter.line('日志透传结束');
    }
    if (this.interactive && nowSeconds - this.lastStat >= 5.0) {
      this.lastStat = nowSeconds;
      let stats = `已转发 ${this.reports} 帧报告，收到设备帧 ${this.frames} 个，`
                + `写回手柄 ${this.outputs} 条`;
      if (this.capture !== null) {
        stats += `，采集已落盘 ${this.capture.count} 条`;
      } else if (this.captured) {
        stats += `，收到采集帧 ${this.captured} 个（:capture <路径> 落盘）`;
      }
      this.reporter.line(stats);
    }
  }

  /** 交互会话主循环；图形界面传 readStdin=false，命令由界面塞进队列。 */
  async runInteractive(readStdin = true) {
    this.interactive = true;
    this.forward = this.args.pad || !this.args.noPad;
    this.reporter.line('会话已连接：输入按固件 CLI 发送；: 开头走工具命令，:help 看清单');
    if (readStdin) {
      this.startStdin();
    }
    while (!this.stop) {
      const at = now();
      try {
        await this.pump(at);
      } catch (exc) {
        this.reporter.error(`链路错误：${exc.message}`);
        this.reporter.event('link_error', { message: exc.message });
        return 1;
      }
      await sleep(0.001);
    }
    return this.stopCode;
  }

  /** 发一条设备命令并等回复安静下来；收到任何行都返回 true。
     *
     * holdFullWindow 用于应答不在 CLI 任务上、晚几拍才回的命令（mem 的报告由
     * UI 任务下一帧打印）：这类命令禁用安静窗提前退出，等满 --reply-wait。
     */
  async queryOnce(command, holdFullWindow = false) {
    this.link.purgeInput();
    this.decoder = new FrameDecoder();
    this.replySeen = false;
    const deadline = now() + this.args.replyWait;
    this.replyDeadline = deadline;
    this.replyHardDeadline = deadline;
    this.sendCli(command);
    while (!this.stop) {
      const at = now();
      const quietExpired = this.replySeen && at >= this.replyDeadline;
      if (at >= deadline || (quietExpired && !holdFullWindow)) {
        break;
      }
      await this.pump(at);
      await sleep(0.001);
    }
    return this.replySeen;
  }

  /** 一次性命令：发一条、等回复、退出（桥接转发同时照跑）。 */
  async runCommand(command) {
    this.oneShot = true;
    this.expectReply = true;
    this.forward = this.args.pad && !this.args.noPad;
    // mem 的应答由 owner task 下一帧才打印，等满整个窗口再收尾。
    const hold = command.split(/\s+/)[0] === 'mem';
    if (!(await this.queryOnce(command, hold))) {
      this.reporter.error(`${this.args.replyWait.toFixed(1)} 秒内没有等到命令回复`);
      return 1;
    }
    return 0;
  }

  /** 一键拉取设备全部观测数据：数据全部由固件现场读取，每个 getter 发一条、
     * 等回复安静。 */
  async runAll() {
    this.oneShot = true;
    this.expectReply = true;
    this.forward = this.args.pad && !this.args.noPad;
    const missing = [];
    for (const command of ALL_QUERIES) {
      this.reporter.line(`--- ${command} ${'-'.repeat(Math.max(0, 56 - command.length))}`);
      const hold = command === 'mem'; // mem 的应答下一帧才回，等满窗口。
      if (!(await this.queryOnce(command, hold))) {
        missing.push(command);
      }
    }
    if (missing.length) {
      this.reporter.error(`没有回复的命令：${missing.join(' ')}`);
      return 1;
    }
    return 0;
  }

  /** 只读日志：--reset 先脉冲复位，从启动日志开始读。 */
  async runLog(seconds, reset, raw) {
    this.forward = this.args.pad && !this.args.noPad;
    if (reset) {
      await this.link.pulseReset();
      this.lineBuf = '';
      this.decoder = new FrameDecoder();
    }
    const started = now();
    const deadline = seconds > 0 ? started + seconds : 0.0;
    this.logMode = true;
    this.logRaw = raw;
    this.logStarted = started;
    while (!this.stop) {
      const at = now();
      if (deadline && at >= deadline) {
        break;
      }
      await this.pump(at);
      await sleep(0.001);
    }
    return 0;
  }

  async runShot(path) {
    this.forward = this.args.pad && !this.args.noPad;
    this.requestShot(path);
    const deadline = now() + this.args.shotTimeout;
    while (!this.stop && !this.shotReady) {
      const at = now();
      if (at >= deadline) {
        this.reporter.error(`${this.args.shotTimeout.toFixed(0)} 秒内没有收到完整截图`);
        return 1;
      }
      await this.pump(at);
      await sleep(0.001);
    }
    return this.shotReady ? 0 : 1;
  }

  /** 升级：状态机在 pump 里推进，桥接转发不断。 */
  async runUpgrade() {
    this.forward = this.args.pad && !this.args.noPad;
    this.requestUpgrade(null);
    while (!this.stop) {
      await this.pump(now());
      await sleep(0.001);
    }
    return this.stopCode;
  }

  /** 上传 amiibo：状态机在 pump 里推进，完成即退出。 */
  async runAmiibo(path) {
    this.forward = this.args.pad && !this.args.noPad;
    this.requestAmiiboUpload(path);
    while (this.amiibo !== null && !this.stop) {
      await this.pump(now());
      await sleep(0.001);
    }
    return this.stopCode;
  }

  /** 抓主机原始输出到文件：--seconds 控制时长（0 = 到 Ctrl+C），
     * 手柄转发照常（--pad / 交互默认开）。 */
  async runCapture(path, seconds) {
    this.forward = this.args.pad && !this.args.noPad;
    this.startCapture(path);
    const deadline = seconds > 0 ? now() + seconds : 0.0;
    while (!this.stop) {
      const at = now();
      if (deadline && at >= deadline) {
        break;
      }
      await this.pump(at);
      await sleep(0.001);
    }
    this.stopCapture();
    return 0;
  }
}

/** ~ 与环境变量开头的路径展开保持最小实现：只处理 ~ 前缀（Windows 常用场景）。 */
export function expandUser(path) {
  if (path === '~' || path.startsWith('~/') || path.startsWith('~\\')) {
    const home = process.env.USERPROFILE ?? process.env.HOME ?? '';
    return home + path.slice(1);
  }
  return path;
}

export function printLocalHelp(reporter) {
  for (const text of [
    '本工具命令：',
    '  :help              显示这份清单',
    '  :all               拉取设备全部观测数据（status/mem/link/... 一键轮询）',
    '  :shot [路径]       抓实机截图并存成 PNG（默认 pc/shots/）',
    '  :log [秒|off]      透传设备日志（0 表示持续到 :log off）',
    '  :ota [镜像路径]    推固件镜像（默认 firmware/build/remapad_firmware.bin）',
    '  :amiibo <bin 路径> 上传 amiibo 镜像到设备（之后用固件命令 amiibo select 选用）',
    '  :capture [路径|off] 抓主机原始输出到文件（震动/玩家灯/指令，布局转换前）',
    '  :quit              退出',
    '其余行按固件 CLI 原样发送。手柄功能的完整控制面都在固件 CLI 里：',
    '  输入注入 key/stick，身份 ctrl，连接 connect/pairing/wake/adv/drop，',
    '  上报内容 motion/headset/fwver/fwpost/fwack/fwapply，链路 ltk/relay，',
    '  反馈测试 rumble/lamp/haptic，屏幕 ui/backlight/screen，模式 mode。',
  ]) {
    reporter.line(text);
  }
}

/** 等设备重启回来，问一次 version 命令并打印。 */
export async function waitForVersion(port, baud, reporter = null) {
  const out = reporter ?? new ConsoleReporter();
  const deadline = now() + REOPEN_TIMEOUT_S;
  while (now() < deadline) {
    await sleep(REOPEN_INTERVAL_S);
    let ser;
    try {
      ser = new SerialLink(port, baud, 100);
    } catch {
      continue;
    }
    try {
      ser.purgeInput();
      ser.write(Buffer.from('version\r'));
      ser.flush();
      const quitAt = now() + 5.0;
      while (now() < quitAt) {
        const line = ser.readline().toString('utf8').trim();
        if (line.startsWith('fw=')) {
          out.line(`设备已回到 COM 口：${line}`);
          return 0;
        }
      }
    } finally {
      ser.close();
    }
  }
  out.error(`${REOPEN_TIMEOUT_S.toFixed(0)} 秒内没有等到设备回到 ${port}`);
  return 1;
}
