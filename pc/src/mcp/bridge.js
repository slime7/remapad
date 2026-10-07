// MCP 工具面与会话线程之间的桥：链路写全部经由会话线程，工具调用串行化。
// 连接是显式动作：服务启动不碰设备，remapad_connect 建链后其余工具才可用。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { readFile, stat } from 'node:fs/promises';

import { openHint } from '../link/index.js';
import { UdpLink, parseEndpoint } from '../link/net.js';
import { SerialLink } from '../link/serial.js';
import { Session } from '../session/session.js';
import { defaultShotPath } from '../session/shot.js';
import { maskSecrets } from '../link/secrets.js';
import { SyncEvent, now, sleep } from '../util.js';
import { KeyStateEngine, ReplayProgress, compileReplay, runReplay } from './timeline.js';
import { MAX_REPLAY_BYTES, McpError, REPLY_LONG_QUIET, REPLY_SHORT_QUIET } from './engine.js';

/** 设备文本行收集器：供工具调用按静默窗收取 CLI 回复；噪音行不入收集。 */
export class ReplySink {
  static NOISE_PREFIXES = ['设备在线', '已转发', '反馈', '（已合并', '会话已连接',
    'ok key injected', 'ok keys released', 'ok stick set', 'ok sticks centered'];

  constructor() {
    this._lines = [];
  }

  line(text) {
    this._append(text);
  }

  error(text) {
    this._append(text);
  }

  event(_name, _fields) {}

  _append(text) {
    if (ReplySink.NOISE_PREFIXES.some((prefix) => text.startsWith(prefix))) {
      return;
    }
    this._lines.push([now(), maskSecrets(text)]);
  }

  mark() {
    return this._lines.length;
  }

  /** 收取 mark 之后的行：任何行推静默窗，ok/err/pong 短窗，硬截止兜底。 */
  async collect(mark, waitS) {
    const hard = now() + waitS;
    let quietDeadline = 0.0;
    for (;;) {
      const at = now();
      const pending = this._lines.slice(mark);
      if (pending.length) {
        const last = pending.at(-1)[1];
        const window = last.startsWith('ok') || last.startsWith('err') || last.startsWith('pong')
          ? REPLY_SHORT_QUIET : REPLY_LONG_QUIET;
        quietDeadline = at + window;
      }
      if ((pending.length && at >= quietDeadline) || at >= hard) {
        break;
      }
      await sleep(0.005);
    }
    const lines = this._lines.slice(mark).map(([, text]) => text);
    this._lines.length = mark;
    return lines;
  }
}

/** 挂上按键引擎的会话：在主循环里钩引擎的命令下发，串口唯一写者仍只在会话线程。 */
class EngineSession extends Session {
  constructor(engine, args, hid, conn, reporter) {
    super(args, hid, conn, reporter);
    this._engine = engine;
    this.pumpHook = (nowSeconds) => this._engine.tick(nowSeconds, this);
  }
}

export class PadBridge {
  constructor(ctrlArgs, scriptMaxMs, replayMaxMs, pingPeriodS = 1.0) {
    this.ctrlArgs = ctrlArgs;
    this.scriptMaxMs = scriptMaxMs;
    this.replayMaxMs = replayMaxMs;
    this.engine = new KeyStateEngine(pingPeriodS);
    this.sink = new ReplySink();
    /** 工具调用串行化：JS 单线程下只需把关键段排在同一条 promise 链上。 */
    this._queueTail = Promise.resolve();
    this.session = null;
    this.worker = null;
    this._workerAlive = false;
    this.replay = null;
    this._replayStop = null;
    this._replayTask = null;
    this._scriptRunning = false;
  }

  /** 把一次工具调用排进串行队列，完成前下一个调用排队等待。 */
  serialize(task) {
    const run = this._queueTail.then(task, task);
    this._queueTail = run.catch(() => {});
    return run;
  }

  async connect(port, net) {
    return this.serialize(() => this._connect(port, net));
  }

  async _connect(port, net) {
    if (this.session !== null && this._workerAlive) {
      throw new McpError('设备已连接，先 remapad_disconnect 再换链路');
    }
    const targetPort = port || this.ctrlArgs.port;
    const targetNet = net || this.ctrlArgs.net;
    if (Boolean(targetPort) === Boolean(targetNet)) {
      throw new McpError('需要 port（串口号，如 COM12）或 net（设备IP[:端口]）恰好一个');
    }
    let conn;
    let linkDesc;
    try {
      if (net) {
        const [host, portNum] = parseEndpoint(net);
        conn = new UdpLink(host, portNum);
        linkDesc = `${host}:${portNum}（WiFi netlog）`;
      } else {
        // 直接构造 SerialLink：打开失败转成 McpError，不让进程退出。
        conn = new SerialLink(port, this.ctrlArgs.baud);
        linkDesc = `${port}（串口）`;
      }
    } catch (exc) {
      throw new McpError(`打开设备链路失败：${exc.message}\n${openHint(exc)}`);
    }
    const session = new EngineSession(this.engine, this.ctrlArgs, null, conn, this.sink);
    this.session = session;
    this.worker = this._runSession(session, conn);
    return { ok: true, link: linkDesc };
  }

  workerAlive() {
    return this._workerAlive;
  }

  /** 断开设备链路：先打断进行中的回放，会话收尾会 key release 全松并关链路。 */
  async disconnect() {
    const progress = this.replay;
    if (progress !== null && progress.snapshot().active) {
      this._replayStop.set();
      await Promise.race([this._replayTask, sleep(3)]);
    }
    const [session, worker] = [this.session, this.worker];
    this.session = null;
    this.worker = null;
    if (session === null) {
      return { ok: true, link: '未连接' };
    }
    this.engine.clear();
    session.stop = true;
    if (worker !== null) {
      await Promise.race([worker, sleep(3)]);
    }
    return { ok: true, link: '已断开', engine: this.engine.state() };
  }

  /** 进程退出钩子：静默断开。 */
  async stop() {
    try {
      await this.disconnect();
    } catch {
      // 收尾路径不抛。
    }
  }

  /** 当前活跃会话；未连接或会话已死一律报错，不隐式重连。 */
  _session() {
    const session = this.session;
    if (session === null || !this._workerAlive) {
      throw new McpError('设备未连接：先调用 remapad_connect（port=串口号 或 net=IP:端口）');
    }
    return session;
  }

  async _runSession(session, conn) {
    this._workerAlive = true;
    try {
      await session.runInteractive(false);
    } catch (exc) {
      this.sink.error(`会话异常结束：${exc.message}`);
    } finally {
      this._workerAlive = false;
      // 泵循环可能因 stop 直接退出，收尾不依赖脏标记：直接下发全松与回中。
      try {
        session.sendCli('key release');
        session.sendCli('stick reset');
      } catch {
        // 链路已坏时尽力而为。
      }
      try {
        conn.close();
      } catch {
        // 收尾路径不抛。
      }
    }
  }

  /** 发一条固件 CLI 并收回复行：命令经会话队列落线程，回复按静默窗收集。 */
  async query(command, waitS = null) {
    return this.serialize(async () => {
      const session = this._session();
      const mark = this.sink.mark();
      session.commands.push(command);
      return this.sink.collect(mark, waitS ?? this.ctrlArgs.replyWait);
    });
  }

  async screenshot(path) {
    return this.serialize(async () => {
      const session = this._session();
      if (session.link instanceof UdpLink) {
        throw new McpError('截图只支持串口会话（netlog UDP 通道不回传图像帧）');
      }
      const target = path ? expandUser(path) : defaultShotPath();
      session.shotPath = target;
      session.shotReady = false;
      session.shot.begin(now(), session.args.shotTimeout);
      session.commands.push('shot');
      const deadline = now() + session.args.shotTimeout;
      while (!session.shotReady && now() < deadline) {
        await sleep(0.02);
      }
      if (!session.shotReady) {
        throw new McpError(`${session.args.shotTimeout.toFixed(0)} 秒内没有收到完整截图`);
      }
      return { path: session.shotPath };
    });
  }

  /** 当前回放任务的状态快照；从未启动过时是 active=False 的空态。 */
  replaySnapshot() {
    return this.replay !== null ? this.replay.snapshot() : { active: false };
  }

  /** 回放进行中拒绝按键类工具：报出进度与打断入口，防止误发按键或误打断。 */
  checkNoReplay() {
    const snap = this.replaySnapshot();
    if (!snap.active) {
      return;
    }
    throw new McpError(`回放进行中：${snap.path}（第 ${snap.loop_index + 1}/${snap.loop} 轮，`
            + `帧 ${snap.frame}/${snap.frames}），期间不接受按键指令；`
            + '要打断请调用 remapad_replay_stop，进度可用 remapad_status 查看');
  }

  /** 脚本与回放共用引擎：占住脚本槽，与并发脚本、进行中回放互斥。 */
  claimScript() {
    if (this._scriptRunning) {
      throw new McpError('已有 remapad_script 在执行，等它结束再发起');
    }
    this.checkNoReplay();
    this._scriptRunning = true;
  }

  releaseScript() {
    this._scriptRunning = false;
  }

  /** 后台启动一次回放：读文件、编时间线、占回放槽起任务，立即返回计划摘要。 */
  async startReplay(path, loop) {
    return this.serialize(async () => {
      const text = await readReplayFile(path);
      const [events, spanMs, frames] = compileReplay(text, loop, this.replayMaxMs);
      this._session();
      if (this._scriptRunning) {
        throw new McpError('remapad_script 正在执行，等它结束再启动回放');
      }
      this.checkNoReplay();
      const progress = new ReplayProgress(String(path), loop, spanMs, frames);
      const stopEvent = new SyncEvent();
      this.replay = progress;
      this._replayStop = stopEvent;
      this._replayTask = (async () => {
        try {
          await runReplay(this.engine, events, loop, spanMs, stopEvent, progress);
        } finally {
          progress.finish(stopEvent.isSet());
        }
      })();
      return {
        ok: true,
        path: String(path),
        events: events.length,
        frames,
        loop,
        span_ms: Math.round(spanMs),
        total_ms: Math.round(spanMs * loop),
      };
    });
  }

  /** 打断进行中的回放：置停止位、等收尾（全松回中），返回结束快照。 */
  async stopReplay() {
    const progress = this.replay;
    if (progress === null || !progress.snapshot().active) {
      return { ok: true, stopped: false, replay: this.replaySnapshot() };
    }
    this._replayStop.set();
    if (this._replayTask !== null) {
      await Promise.race([this._replayTask, sleep(5)]);
    }
    return { ok: true, stopped: true, replay: progress.snapshot() };
  }
}

/** 读回放文件：UTF-8 文本，4 MiB 上限。 */
async function readReplayFile(path) {
  const target = expandUser(path);
  let size;
  try {
    size = (await stat(target)).size;
  } catch (exc) {
    throw new McpError(`回放文件不可读：${exc.message}`);
  }
  if (size > MAX_REPLAY_BYTES) {
    throw new McpError(`回放文件 ${size} 字节超过上限 ${MAX_REPLAY_BYTES}`);
  }
  try {
    const raw = await readFile(target, 'utf8');
    return raw.replace(/^\ufeff/, ''); // UTF-8 BOM 容忍（与 Python utf-8-sig 同语义）
  } catch (exc) {
    throw new McpError(`回放文件读取失败（需 UTF-8 文本）：${exc.message}`);
  }
}

/** ~ 开头的路径展开保持最小实现。 */
export function expandUser(path) {
  if (path === '~' || path.startsWith('~/') || path.startsWith('~\\')) {
    return (process.env.USERPROFILE ?? process.env.HOME ?? '') + path.slice(1);
  }
  return path;
}
