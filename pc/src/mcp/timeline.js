// MCP 按键服务的注入引擎：自持按键状态的 key/stick CLI 下发与脚本/回放时间线。
// 数据与核对状态见 docs/ABSTRACTIONS.md。

import { TYPE_PING, encode } from '../link/frame.js';
import { UdpLink } from '../link/net.js';
import { now, sleep } from '../util.js';
import {
  DEFAULT_FRAME_MS,
  HOLD_REFRESH_MS,
  HOLD_SLICE_MS,
  KEY_NAMES,
  MAX_HOLD_MS,
  MAX_REPLAY_EVENTS,
  MAX_SCRIPT_ACTIONS,
  MAX_SCRIPT_EVENTS,
  McpError,
  REPLAY_SLICE_S,
  STICK_CENTER,
  validateKeys,
  validateStickXy,
} from './engine.js';

/** 自持按键状态的注入引擎：下发固件调试 CLI 的 key/stick 命令。
 *
 * 不变式：固件注入掩码 = 活跃键全集，倒计时 = 最小剩余期限。因此任何增删都把
 * 活跃键按剩余时长降序逐条重发（最后一条的倒计时即最早期限）；tap 在无按住键时
 * 直发（固件到点自动松开，零状态零流量），与按住键并存时作为有限期限入表。
 * 固件没有单键松开，子集松开只能 `key release` 全松后重发存活键。状态变更来自
 * 工具调用；命令发送只允许会话线程（串口唯一写者），由 tick 驱动。
 */
export class KeyStateEngine {
  constructor(pingPeriodS = 1.0) {
    this._held = new Map(); // 键名 → [期限 monotonic 秒, 是否滚动续期]
    this._stick = {
      l: [STICK_CENTER, STICK_CENTER],
      r: [STICK_CENTER, STICK_CENTER],
    };
    this._now = null; // 最近一次 tick 的时钟：工具调用据此打期限戳
    this._sent = new Set(); // 最近一次下发的固件掩码
    this._stickSent = { l: [STICK_CENTER, STICK_CENTER], r: [STICK_CENTER, STICK_CENTER] };
    this._keysDirty = false;
    this._stickDirty = false;
    this._releasePending = false; // 用户侧松空了按键表：固件掩码还在，需要显式 key release
    this._lastPing = 0.0;
    this.pingPeriodS = pingPeriodS;
  }

  _stamp() {
    return this._now ?? now();
  }

  // --- 状态变更（工具调用） ---------------------------------------

  tap(names, holdMs) {
    const deadline = this._stamp() + holdMs / 1000.0;
    for (const name of names) {
      this._held.set(name, [deadline, false]);
    }
    this._keysDirty = true;
  }

  press(names) {
    const added = names.filter((name) => !this._held.has(name));
    for (const name of added) {
      this._held.set(name, [this._stamp() + HOLD_SLICE_MS / 1000.0, true]);
    }
    if (added.length) {
      this._keysDirty = true;
    }
    return added;
  }

  release(names) {
    const removed = names.filter((name) => this._held.has(name));
    for (const name of removed) {
      this._held.delete(name);
    }
    if (removed.length) {
      this._keysDirty = true;
      if (!this._held.size) {
        this._releasePending = true;
      }
    }
    return removed;
  }

  setStick(side, x, y) {
    this._stick[side] = [x, y];
    this._stickDirty = true;
  }

  resetStick() {
    this._stick = {
      l: [STICK_CENTER, STICK_CENTER],
      r: [STICK_CENTER, STICK_CENTER],
    };
    this._stickDirty = true;
  }

  /** 全部松开并回中：逃生口与脚本收尾共用（也提前终止在按的 tap）。 */
  clear() {
    const held = [...this._held.keys()];
    this._held.clear();
    this._stick = {
      l: [STICK_CENTER, STICK_CENTER],
      r: [STICK_CENTER, STICK_CENTER],
    };
    if (held.length) {
      this._keysDirty = true;
      this._releasePending = true;
    }
    this._stickDirty = true;
    return held;
  }

  state() {
    return {
      held: [...this._held.keys()],
      stick: { l: [...this._stick.l], r: [...this._stick.r] },
    };
  }

  // --- 命令发送（仅会话线程） -------------------------------------

  /** 会话主循环驱动：UDP 保活、到期剪枝、滚动续期与脏状态下发。 */
  tick(nowSeconds, session) {
    this._now = nowSeconds;
    if (session.link instanceof UdpLink && nowSeconds - this._lastPing >= this.pingPeriodS) {
      session.link.write(encode(TYPE_PING, 0, session.seq, Buffer.alloc(0)));
      session.seq = (session.seq + 1) & 0xff;
      this._lastPing = nowSeconds;
    }
    const expired = [];
    for (const [name, entry] of this._held) {
      if (!entry[1] && nowSeconds >= entry[0]) {
        expired.push(name);
      }
    }
    for (const name of expired) {
      this._held.delete(name);
    }
    if (expired.length) {
      // 固件倒计时归零时清掉整个掩码，存活键要重发，已发集合随之清空。
      this._sent = new Set();
      this._keysDirty = true;
    }
    const refreshed = [];
    for (const [name, entry] of this._held) {
      if (entry[1] && entry[0] - nowSeconds < HOLD_REFRESH_MS / 1000.0) {
        refreshed.push(name);
      }
    }
    for (const name of refreshed) {
      this._held.set(name, [nowSeconds + HOLD_SLICE_MS / 1000.0, true]);
    }
    if (refreshed.length) {
      this._keysDirty = true;
    }
    if (this._keysDirty) {
      this._flushKeys(session, nowSeconds);
    }
    if (this._stickDirty) {
      this._flushStick(session);
    }
  }

  _flushKeys(session, nowSeconds) {
    const entries = [...this._held.entries()]
      .map(([name, [deadline]]) => [deadline, name])
      .sort((a, b) => b[0] - a[0]);
    const releasePending = this._releasePending;
    const names = new Set(entries.map(([, name]) => name));
    const stale = [...this._sent].filter((name) => !names.has(name));
    this._keysDirty = false;
    this._releasePending = false;
    if (!entries.length) {
      // 自然到期（tap 到点）固件已自行清掉掩码，不必多发；用户松空才显式全松。
      if (releasePending) {
        session.sendCli('key release');
        this._sent = new Set();
      }
      return;
    }
    // 掩码按位或、单键清不掉：下发集合有缩小时先 key release 再重发，
    // 随后按剩余降序逐条发（最后一条的倒计时 = 最早期限）。
    if (stale.length || releasePending) {
      session.sendCli('key release');
    }
    for (const [deadline, name] of entries) {
      const remaining = Math.max(1, Math.round((deadline - nowSeconds) * 1000));
      session.sendCli(`key ${name} ${Math.min(remaining, MAX_HOLD_MS)}`);
    }
    this._sent = new Set(names);
  }

  _flushStick(session) {
    const stick = { l: [...this._stick.l], r: [...this._stick.r] };
    const sent = this._stickSent;
    this._stickDirty = false;
    const centered = (levels) => levels[0] === STICK_CENTER && levels[1] === STICK_CENTER;
    if (centered(stick.l) && centered(stick.r)) {
      if (sent === null || !centered(sent.l) || !centered(sent.r)) {
        session.sendCli('stick reset');
        this._stickSent = { l: [...stick.l], r: [...stick.r] };
      }
      return;
    }
    for (const side of ['l', 'r']) {
      if (sent === null || sent[side][0] !== stick[side][0] || sent[side][1] !== stick[side][1]) {
        session.sendCli(`stick ${side} ${stick[side][0]} ${stick[side][1]}`);
      }
    }
    this._stickSent = { l: [...stick.l], r: [...stick.r] };
  }
}

/** 把 actions 编译成事件时间线：tap 展开成 down/up 对，按 (t, 种类) 稳定排序。
 * 返回 [events, spanMs]；同刻事件的执行顺序为 up → down → stick。 */
export function compileScript(actions, loop, maxMs) {
  if (!Array.isArray(actions) || !actions.length) {
    throw new McpError('actions 必须是非空数组');
  }
  if (actions.length > MAX_SCRIPT_ACTIONS) {
    throw new McpError(`actions 最多 ${MAX_SCRIPT_ACTIONS} 项，收到 ${actions.length} 项`);
  }
  if (typeof loop !== 'number' || !Number.isInteger(loop) || loop < 1 || loop > 1000) {
    throw new McpError('loop 必须是 1-1000 的整数');
  }
  const events = [];
  for (const [index, action] of actions.entries()) {
    if (action === null || typeof action !== 'object' || Array.isArray(action)) {
      throw new McpError(`actions[${index}] 必须是对象`);
    }
    const atMs = action.t ?? 0;
    if (typeof atMs !== 'number' || !Number.isFinite(atMs) || atMs < 0) {
      throw new McpError(`actions[${index}].t 必须是不小于 0 的毫秒数`);
    }
    for (const name of validateKeys(action.up ?? [], { allowEmpty: true })) {
      events.push([atMs, 0, 'up', name]);
    }
    for (const name of validateKeys(action.down ?? [], { allowEmpty: true })) {
      events.push([atMs, 1, 'down', name]);
    }
    const tapKeys = validateKeys(action.tap ?? [], { allowEmpty: true });
    const holdMs = action.hold_ms ?? 200;
    if (typeof holdMs !== 'number' || !Number.isFinite(holdMs) || holdMs < 1 || holdMs > MAX_HOLD_MS) {
      throw new McpError(`actions[${index}].hold_ms 必须是 1-${MAX_HOLD_MS} 的毫秒数`);
    }
    for (const name of tapKeys) {
      events.push([atMs, 1, 'down', name]);
      events.push([atMs + holdMs, 0, 'up', name]);
    }
    const stick = action.stick;
    if (stick !== undefined && stick !== null) {
      const side = stick.side;
      if (side !== 'l' && side !== 'r') {
        throw new McpError(`actions[${index}].stick.side 必须是 l 或 r`);
      }
      const [x, y] = validateStickXy(stick.x, stick.y);
      events.push([atMs, 2, 'stick', [side, x, y]]);
    }
    if (action.stick_reset) {
      events.push([atMs, 2, 'stick_reset', null]);
    }
    if (events.length > MAX_SCRIPT_EVENTS) {
      throw new McpError(`展开后的事件数超过 ${MAX_SCRIPT_EVENTS}`);
    }
  }
  if (!events.length) {
    throw new McpError('actions 里没有可执行的动作字段（down/up/tap/stick/stick_reset）');
  }
  events.sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));
  const spanMs = Math.max(...events.map((event) => event[0]));
  const totalMs = spanMs * loop;
  if (totalMs > maxMs) {
    throw new McpError(`脚本总时长 ${totalMs.toFixed(0)}ms 超过上限 ${maxMs}ms（可调 --script-max-ms）`);
  }
  return [events, spanMs];
}

/** 按时间线驱动引擎：阻塞执行，结束（含异常路径）松开全部并回中。返回耗时 ms。 */
export async function runScript(engine, events, loop) {
  const started = now();
  try {
    for (let iteration = 0; iteration < loop; iteration++) {
      const repStarted = now();
      for (const [atMs, _order, kind, payload] of events) {
        const delay = repStarted + atMs / 1000.0 - now();
        if (delay > 0) {
          await sleep(delay);
        }
        if (kind === 'down') {
          engine.press([payload]);
        } else if (kind === 'up') {
          engine.release([payload]);
        } else if (kind === 'stick') {
          engine.setStick(payload[0], payload[1], payload[2]);
        } else {
          engine.resetStick();
        }
      }
    }
  } finally {
    engine.clear();
  }
  return (now() - started) * 1000.0;
}

function parseReplayNumber(value, lineNo, key) {
  const number = Number(value.trim());
  if (!Number.isFinite(number) || number <= 0) {
    throw new McpError(`第 ${lineNo} 行头部 ${key} 必须是正数，收到 ${JSON.stringify(value)}`);
  }
  return number;
}

function parseReplayButtons(text, lineNo) {
  const trimmed = text.trim();
  if (trimmed === '' || trimmed === '.' || trimmed === '-') {
    return [];
  }
  try {
    return validateKeys(trimmed.split('+').filter(Boolean), { allowEmpty: true, maxKeys: KEY_NAMES.length });
  } catch (exc) {
    throw new McpError(`第 ${lineNo} 行按键字段：${exc.message}`);
  }
}

function parseReplayStick(text, lineNo) {
  const trimmed = text.trim();
  if (trimmed === '' || trimmed === '.' || trimmed === '-') {
    return null;
  }
  const parts = trimmed.split(',');
  if (parts.length !== 2) {
    throw new McpError(`第 ${lineNo} 行摇杆必须是 x,y（0-4095）或 .，收到 ${JSON.stringify(text)}`);
  }
  const x = Number(parts[0]);
  const y = Number(parts[1]);
  if (!Number.isFinite(x) || !Number.isFinite(y)) {
    throw new McpError(`第 ${lineNo} 行摇杆必须是 x,y（0-4095）或 .，收到 ${JSON.stringify(text)}`);
  }
  try {
    return validateStickXy(x, y);
  } catch (exc) {
    throw new McpError(`第 ${lineNo} 行摇杆：${exc.message}`);
  }
}

function parseReplayFrame(line, lineNo) {
  let fields = line.split('|');
  if (fields[0] === '') {
    fields = fields.slice(1);
  }
  if (fields.at(-1) === '') {
    fields = fields.slice(0, -1);
  }
  if (fields.length > 4) {
    throw new McpError(`第 ${lineNo} 行帧字段必须是 |帧号|按键|左摇杆|右摇杆|，收到 ${JSON.stringify(line)}`);
  }
  while (fields.length < 4) {
    fields.push('.');
  }
  const frameText = fields[0].trim();
  if (!/^\d+$/.test(frameText)) {
    throw new McpError(`第 ${lineNo} 行帧号必须是非负整数，收到 ${JSON.stringify(frameText)}`);
  }
  return [Number(frameText), parseReplayButtons(fields[1], lineNo),
    parseReplayStick(fields[2], lineNo), parseReplayStick(fields[3], lineNo)];
}

/** 解析 TAS 式逐帧记录：头部 key = value（frame_ms / fps）加 |帧号|按键|左摇杆|右摇杆| 帧行。
 * 帧行表示「从该帧起的输入状态」并保持到下一帧行；返回 [frameMs, segments]。 */
export function parseReplay(text) {
  let frameMs = DEFAULT_FRAME_MS;
  const segments = [];
  let lastFrame = -1;
  const lines = text.split(/\r?\n/);
  for (const [index, raw] of lines.entries()) {
    const lineNo = index + 1;
    const line = raw.trim();
    if (!line || line.startsWith('#')) {
      continue;
    }
    if (line.startsWith('|')) {
      const [frame, buttons, lstick, rstick] = parseReplayFrame(line, lineNo);
      if (frame <= lastFrame) {
        throw new McpError(`第 ${lineNo} 行帧号 ${frame} 必须大于上一行的 ${lastFrame}`);
      }
      lastFrame = frame;
      segments.push([frame, buttons, lstick, rstick]);
      continue;
    }
    const eq = line.indexOf('=');
    if (eq < 0) {
      throw new McpError(`第 ${lineNo} 行无法解析：${JSON.stringify(raw)}（帧行以 | 开头，头部是 key = value）`);
    }
    const key = line.slice(0, eq).trim().toLowerCase();
    const value = line.slice(eq + 1);
    if (key === 'frame_ms') {
      frameMs = parseReplayNumber(value, lineNo, key);
      if (frameMs < 0.5 || frameMs > 1000) {
        throw new McpError(`第 ${lineNo} 行 frame_ms 需在 0.5-1000 之间，收到 ${frameMs}`);
      }
    } else if (key === 'fps') {
      const fps = parseReplayNumber(value, lineNo, key);
      if (fps < 1 || fps > 2000) {
        throw new McpError(`第 ${lineNo} 行 fps 需在 1-2000 之间，收到 ${fps}`);
      }
      frameMs = 1000.0 / fps;
    } else {
      throw new McpError(`第 ${lineNo} 行未知头部键 ${JSON.stringify(key)}（可用 frame_ms / fps）`);
    }
  }
  if (!segments.length) {
    throw new McpError('回放文件没有任何帧行（帧行形如 |0|circle|2048,2048|.|）');
  }
  return [frameMs, segments];
}

/** 把回放记录编成事件时间线（与 compileScript 同构）：返回 [events, spanMs, frames]。
 * 帧号间隔就是保持前一状态的空拍；末帧仍按着的键与偏离中位的摇杆在边界收尾，
 * 保证 loop 重复时每一轮都从中位起手。 */
export function compileReplay(text, loop, maxMs) {
  if (typeof loop !== 'number' || !Number.isInteger(loop) || loop < 1 || loop > 1000) {
    throw new McpError('loop 必须是 1-1000 的整数');
  }
  const [frameMs, segments] = parseReplay(text);
  const events = [];
  let prev = new Set();
  const stick = { l: [STICK_CENTER, STICK_CENTER], r: [STICK_CENTER, STICK_CENTER] };
  for (const [frame, buttons, lstick, rstick] of segments) {
    const atMs = frame * frameMs;
    const current = new Set(buttons);
    for (const name of [...prev].filter((name) => !current.has(name)).sort()) {
      events.push([atMs, 0, 'up', name]);
    }
    for (const name of [...current].filter((name) => !prev.has(name)).sort()) {
      events.push([atMs, 1, 'down', name]);
    }
    for (const [side, value] of [['l', lstick], ['r', rstick]]) {
      if (value !== null && (value[0] !== stick[side][0] || value[1] !== stick[side][1])) {
        events.push([atMs, 2, 'stick', [side, value[0], value[1]]]);
        stick[side] = value;
      }
    }
    prev = current;
  }
  const endMs = (segments.at(-1)[0] + 1) * frameMs;
  for (const name of [...prev].sort()) {
    events.push([endMs, 0, 'up', name]);
  }
  for (const side of ['l', 'r']) {
    if (stick[side][0] !== STICK_CENTER || stick[side][1] !== STICK_CENTER) {
      events.push([endMs, 2, 'stick', [side, STICK_CENTER, STICK_CENTER]]);
    }
  }
  events.sort((a, b) => (a[0] - b[0]) || (a[1] - b[1]));
  if (events.length > MAX_REPLAY_EVENTS) {
    throw new McpError(`展开后的事件数超过 ${MAX_REPLAY_EVENTS}`);
  }
  const frames = segments.at(-1)[0] + 1;
  const spanMs = frames * frameMs;
  if (spanMs * loop > maxMs) {
    throw new McpError(`回放总时长 ${spanMs * loop}ms 超过上限 ${maxMs}ms（可调 --replay-max-ms）`);
  }
  return [events, spanMs, frames];
}

/** 一条回放任务的共享进度：回放推进，工具调用经 snapshot 读取。 */
export class ReplayProgress {
  constructor(path, loop, spanMs, frames) {
    this._path = path;
    this._loop = loop;
    this._spanMs = spanMs;
    this._frames = frames;
    this._started = now();
    this._active = true;
    this._interrupted = false;
    this._loopIndex = 0;
    this._frame = 0;
    this._elapsedMs = 0;
  }

  /** 回放每执行完一个事件推进一次：轮次、帧位与墙钟耗时。 */
  update(loopIndex, atMs) {
    const frame = this._spanMs ? Math.trunc(atMs / (this._spanMs / this._frames)) : 0;
    this._loopIndex = loopIndex;
    this._frame = Math.max(0, Math.min(this._frames - 1, frame));
    this._elapsedMs = Math.trunc((now() - this._started) * 1000);
  }

  /** 收尾：落定结束态并冻结耗时。 */
  finish(interrupted) {
    this._active = false;
    this._interrupted = interrupted;
    this._elapsedMs = Math.trunc((now() - this._started) * 1000);
  }

  snapshot() {
    return {
      active: this._active,
      path: this._path,
      loop: this._loop,
      loop_index: this._loopIndex,
      frame: this._frame,
      frames: this._frames,
      elapsed_ms: this._elapsedMs,
      total_ms: Math.round(this._spanMs * this._loop),
      interrupted: this._interrupted,
    };
  }
}

/** 分段睡到目标时刻；期间 stopEvent 置位即返回 true（未到点先打断）。 */
async function sleepUntil(target, stopEvent) {
  for (;;) {
    const delay = target - now();
    if (delay <= 0) {
      return false;
    }
    if (await stopEvent.wait(Math.min(delay, REPLAY_SLICE_S))) {
      return true;
    }
  }
}

/** 后台按时间线驱动引擎：stopEvent 置位即打断，结束（含打断）松开全部并回中。
 * 每轮把末帧保持睡满到 spanMs 再进下一轮，循环周期与记录时长一致。 */
export async function runReplay(engine, events, loop, spanMs, stopEvent, progress) {
  const spanS = spanMs / 1000.0;
  const started = now();
  try {
    for (let iteration = 0; iteration < loop; iteration++) {
      const repStarted = now();
      for (const [atMs, _order, kind, payload] of events) {
        if (await sleepUntil(repStarted + atMs / 1000.0, stopEvent)) {
          return (now() - started) * 1000.0;
        }
        progress.update(iteration, atMs);
        if (kind === 'down') {
          engine.press([payload]);
        } else if (kind === 'up') {
          engine.release([payload]);
        } else if (kind === 'stick') {
          engine.setStick(payload[0], payload[1], payload[2]);
        } else {
          engine.resetStick();
        }
      }
      if (await sleepUntil(repStarted + spanS, stopEvent)) {
        return (now() - started) * 1000.0;
      }
      progress.update(iteration, spanMs);
    }
  } finally {
    engine.clear();
  }
  return (now() - started) * 1000.0;
}
