// MCP 回放：TAS 式记录的解析与编译、回放任务的推进与打断、回放期的按键工具拒绝。
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, beforeEach, describe, expect, it } from 'vitest';

import { parseCtrlArgs } from '../src/args.js';
import { McpError } from '../src/mcp/engine.js';
import { KeyStateEngine, ReplayProgress, compileReplay, parseReplay, runReplay } from '../src/mcp/timeline.js';
import { PadBridge } from '../src/mcp/bridge.js';
import { TOOLS, setBridge } from '../src/mcp/server.js';
import { SyncEvent } from '../src/util.js';

function call(name, args) {
  return TOOLS.find((tool) => tool.name === name).handler(args);
}

class FakeLink {
  constructor() {
    this.written = [];
  }

  write(data) {
    this.written.push(data);
  }

  flush() {}
}

class FakeSession {
  constructor(conn) {
    this.link = conn;
    this.seq = 0;
    this.commands = [];
    this.lines = [];
  }

  sendCli(command) {
    this.lines.push(command);
  }
}

function kinds(events) {
  return events.map(([atMs, _order, kind, payload]) => [atMs, kind, payload]);
}

const tempDirs = [];
afterEach(() => {
  while (tempDirs.length) {
    rmSync(tempDirs.pop(), { recursive: true, force: true });
  }
});

describe('parseReplay', () => {
  it('default frame ms and segments', () => {
    const [frameMs, segments] = parseReplay('|0|circle+cross|3000,2048|.\n|10|up|.|.|\n');
    expect(frameMs).toBeCloseTo(15.0);
    expect(segments).toEqual([
      [0, ['circle', 'cross'], [3000, 2048], null],
      [10, ['up'], null, null],
    ]);
  });

  it('frame ms and fps header with comments', () => {
    let [frameMs, segments] = parseReplay('# 头部注释\r\nframe_ms = 2.5\r\n\r\n|0|circle|.|.|\r\n');
    expect(frameMs).toBe(2.5);
    expect(segments).toEqual([[0, ['circle'], null, null]]);
    [frameMs] = parseReplay('fps = 60\n|0|||\n');
    expect(frameMs).toBeCloseTo(1000.0 / 60.0);
  });

  it('empty fields mean no buttons and unchanged sticks', () => {
    let [, segments] = parseReplay('|4||2048,2048|\n');
    expect(segments).toEqual([[4, [], [2048, 2048], null]]);
    [, segments] = parseReplay('|7|circle|.|.|');
    expect(segments).toEqual([[7, ['circle'], null, null]]);
  });

  it('rejects bad content', () => {
    for (const bad of ['frame_ms = 0\n|0|||', 'frame_ms = 5000\n|0|||', 'wombat = 1\n|0|||',
      '|3|circle|.|.\n|1|||', '|0|nope|.|.|', '|0|circle|9999,0|.|',
      '|0|circle|1,2|x|', '|0|circle|1,2,3|.|', '-2\n|0|||', 'hello\n|0|||',
      '# 只有注释没有帧', '']) {
      expect(() => parseReplay(bad)).toThrow(McpError);
    }
  });
});

describe('compileReplay', () => {
  it('buttons become down up and sticks sticky', () => {
    const [events, span, frames] = compileReplay(
      'frame_ms = 10\n|0|circle|4095,2048|.\n|5||.|.|\n|10|.|2048,2048|.\n', 1, 60000);
    expect(kinds(events)).toEqual([
      [0.0, 'down', 'circle'],
      [0.0, 'stick', ['l', 4095, 2048]],
      [50.0, 'up', 'circle'],
      [100.0, 'stick', ['l', 2048, 2048]],
    ]);
    expect(span).toBe(110.0);
    expect(frames).toBe(11);
  });

  it('gap holds state until next frame line', () => {
    const [events, span, frames] = compileReplay('frame_ms = 10\n|0|circle|.|.\n|30||.|.\n', 1, 60000);
    expect(kinds(events)).toEqual([[0.0, 'down', 'circle'], [300.0, 'up', 'circle']]);
    expect(span).toBe(310.0);
    expect(frames).toBe(31);
  });

  it('loop boundary releases end state', () => {
    const [events, span] = compileReplay('frame_ms = 10\n|0|circle|3000,3000|.\n|20||.|.\n', 3, 60000);
    expect(kinds(events)).toEqual([
      [0.0, 'down', 'circle'], [0.0, 'stick', ['l', 3000, 3000]],
      [200.0, 'up', 'circle'],
      [210.0, 'stick', ['l', 2048, 2048]],
    ]);
    expect(span).toBe(210.0);
  });

  it('duration cap and loop range', () => {
    const text = 'frame_ms = 10\n|0|circle|.|.\n|1||.|.\n';
    expect(() => compileReplay(text, 1, 10)).toThrow(McpError);
    for (const badLoop of [0, -1, 1.5, true, 1001]) {
      expect(() => compileReplay(text, badLoop, 60000)).toThrow(McpError);
    }
    compileReplay(text, 1000, 60000); // 20s 总时长在上限内
  });

  it('event cap', () => {
    const lines = Array.from({ length: 70000 }, (_, i) => `|${i}|${i % 2 ? 'circle' : ''}|.|.|`).join('\n');
    expect(() => compileReplay(`frame_ms = 1\n${lines}\n`, 1, 600000)).toThrow(McpError);
  });
});

describe('runReplay', () => {
  it('runs timeline and clears at end', async () => {
    const engine = new KeyStateEngine();
    const [events, span, frames] = compileReplay('frame_ms = 5\n|0|circle|.|.\n|2||.|.\n', 1, 1000);
    const progress = new ReplayProgress('x.tas', 1, span, frames);
    const duration = await runReplay(engine, events, 1, span, new SyncEvent(), progress);
    expect(duration).toBeGreaterThanOrEqual(span);
    expect(engine.state().held).toEqual([]);
    const snap = progress.snapshot();
    expect(snap.frames).toBe(3);
    expect(snap.elapsed_ms).toBeGreaterThanOrEqual(span);
  });

  it('held key visible mid run and stop interrupts', async () => {
    const engine = new KeyStateEngine();
    const [events, span, frames] = compileReplay('frame_ms = 10\n|0|circle|.|.\n|49||.|.\n', 1, 60000);
    const progress = new ReplayProgress('x.tas', 1, span, frames);
    const stopEvent = new SyncEvent();
    const worker = runReplay(engine, events, 1, span, stopEvent, progress);
    const deadline = performance.now() + 2000;
    while (performance.now() < deadline && engine.state().held.join() !== 'circle') {
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    expect(engine.state().held).toEqual(['circle']);
    stopEvent.set();
    await worker;
    expect(engine.state().held).toEqual([]);
    expect(progress.snapshot().elapsed_ms).toBeLessThan(span);
    void frames;
  });
});

describe('replay bridge', () => {
  function makeBridge() {
    const bridge = new PadBridge(parseCtrlArgs(['--no-pad']), 60000, 600000, 1.0);
    bridge._session = () => new FakeSession(new FakeLink());
    return bridge;
  }

  function makeDir() {
    const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    return dir;
  }

  async function waitActive(bridge, timeout = 2000) {
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
      const snap = bridge.replaySnapshot();
      if (snap.active) {
        return snap;
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    throw new Error('回放没有进入活跃状态');
  }

  async function waitDone(bridge, timeout = 5000) {
    const deadline = performance.now() + timeout;
    while (performance.now() < deadline) {
      const snap = bridge.replaySnapshot();
      if (!snap.active) {
        return snap;
      }
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
    throw new Error('回放没有在期限内结束');
  }

  it('replay lifecycle runs and allows restart', async () => {
    const bridge = makeBridge();
    const dir = makeDir();
    const path = join(dir, 'movie.tas');
    writeFileSync(path, 'frame_ms = 5\n|0|circle|.|.\n|2||.|.\n');
    const plan = await bridge.startReplay(path, 1);
    expect(plan.ok).toBe(true);
    expect(plan.frames).toBe(3);
    expect(plan.span_ms).toBe(15);
    const snap = await waitDone(bridge);
    expect(snap.interrupted).toBe(false);
    expect(bridge.engine.state().held).toEqual([]);
    expect((await bridge.startReplay(path, 1)).ok).toBe(true); // 结束后可再次启动
    await bridge.stopReplay();
    await waitDone(bridge);
  });

  it('start requires connection and slot is exclusive', async () => {
    const bridge = makeBridge();
    const dir = makeDir();
    const path = join(dir, 'movie.tas');
    writeFileSync(path, 'frame_ms = 10\n|0|circle|.|.\n|49||.|.\n');
    const strict = new PadBridge(parseCtrlArgs(['--no-pad']), 60000, 600000, 1.0);
    await expect(strict.startReplay(path, 1)).rejects.toThrow(McpError);
    expect((await bridge.startReplay(path, 1)).ok).toBe(true);
    await waitActive(bridge);
    await expect(bridge.startReplay(path, 1)).rejects.toThrow(McpError);
    await bridge.stopReplay();
    await waitDone(bridge);
  });

  it('stop interrupts and is idempotent', async () => {
    const bridge = makeBridge();
    const dir = makeDir();
    const path = join(dir, 'movie.tas');
    writeFileSync(path, 'frame_ms = 10\n|0|circle|.|.\n|49||.|.\n');
    await bridge.startReplay(path, 1);
    await waitActive(bridge);
    const result = await bridge.stopReplay();
    expect(result.stopped).toBe(true);
    const snap = await waitDone(bridge);
    expect(snap.interrupted).toBe(true);
    expect(bridge.engine.state().held).toEqual([]);
    expect((await bridge.stopReplay()).stopped).toBe(false); // 无回放时幂等成功
  });

  it('disconnect aborts running replay', async () => {
    const bridge = makeBridge();
    const dir = makeDir();
    const path = join(dir, 'movie.tas');
    writeFileSync(path, 'frame_ms = 10\n|0|circle|.|.\n|49||.|.\n');
    await bridge.startReplay(path, 1);
    await waitActive(bridge);
    expect((await bridge.disconnect()).ok).toBe(true);
    const snap = await waitDone(bridge);
    expect(snap.interrupted).toBe(true);
    expect(bridge.engine.state().held).toEqual([]);
  });

  it('rejects unreadable and oversize files', async () => {
    const bridge = makeBridge();
    const dir = makeDir();
    await expect(bridge.startReplay(join(dir, 'missing.tas'), 1)).rejects.toThrow(McpError);
    const big = join(dir, 'big.tas');
    writeFileSync(big, Buffer.alloc(0)); // 占位；真正的大文件用重复行拼出
    writeFileSync(big, '|0|||\n'.repeat(Math.floor(4 * 1024 * 1024 / 6) + 1));
    await expect(bridge.startReplay(big, 1)).rejects.toThrow(McpError);
  });
});

describe('replay tool surface', () => {
  let bridge;
  let dir;

  async function waitActive() {
    const deadline = performance.now() + 2000;
    while (performance.now() < deadline && !bridge.replaySnapshot().active) {
      await new Promise((resolve) => setTimeout(resolve, 5));
    }
  }

  beforeEach(async () => {
    bridge = new PadBridge(parseCtrlArgs(['--no-pad']), 60000, 600000, 1.0);
    bridge._session = () => new FakeSession(new FakeLink());
    bridge.ctrlArgs.replyWait = 0.01;
    setBridge(bridge);
    dir = mkdtempSync(join(tmpdir(), 'remapad-'));
    tempDirs.push(dir);
    const path = join(dir, 'movie.tas');
    writeFileSync(path, 'frame_ms = 10\n|0|circle|.|.\n|49||.|.\n');
    expect((await call('remapad_replay', { path })).ok).toBe(true);
    await waitActive();
  });

  afterEach(async () => {
    await bridge.stopReplay();
    setBridge(null);
  });

  it('key tools refuse with replay hint', async () => {
    expect((await call('remapad_tap', { keys: ['circle'] })).error).toContain('remapad_replay_stop');
    expect((await call('remapad_hold', { keys: ['circle'] })).error).toContain('回放');
    expect((await call('remapad_stick', { side: 'l', x: 0, y: 0 })).error).toContain('回放');
    expect((await call('remapad_stick_reset', {})).error).toContain('回放');
    expect((await call('remapad_script', { actions: [{ t: 0, tap: ['circle'] }] })).error).toContain('回放');
    expect((await call('remapad_release_all', {})).error).toContain('回放');
    expect((await call('remapad_replay', { path: 'another.tas' })).error).toContain('回放');
  });

  it('status reports active replay', async () => {
    const status = await call('remapad_status', {});
    expect(status.ok).toBe(true);
    expect(status.replay.active).toBe(true);
    expect(status.replay.frame).toBeLessThan(status.replay.frames);
    expect(status.replay.loop).toBe(1);
  });
});
