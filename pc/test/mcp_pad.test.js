// MCP 按键服务纯逻辑：键名表、CLI 注入引擎的命令序列、脚本时间线与工具注册面。
import { describe, expect, it } from 'vitest';

import { FrameDecoder, TYPE_PING } from '../src/link/frame.js';
import { UdpLink } from '../src/link/net.js';
import { parseCtrlArgs } from '../src/args.js';
import { KEY_NAMES, MAX_KEYS_PER_CALL, McpError, validateKeys } from '../src/mcp/engine.js';
import { KeyStateEngine, compileScript } from '../src/mcp/timeline.js';
import { PadBridge, ReplySink } from '../src/mcp/bridge.js';
import { listTools, setBridge } from '../src/mcp/server.js';

class FakeLink {
  constructor() {
    this.written = [];
  }

  write(data) {
    this.written.push(data);
  }

  flush() {}
}

/** 借 UdpLink 的类型身份触发引擎的 UDP 保活分支，不真开套接字。 */
function fakeUdpLink() {
  const link = Object.create(UdpLink.prototype);
  link.written = [];
  link.write = (data) => {
    link.written.push(data);
  };
  link.flush = () => {};
  return link;
}

class FakeSession {
  constructor(conn) {
    this.link = conn;
    this.seq = 0;
    this.lines = [];
  }

  sendCli(command) {
    this.lines.push(command);
  }
}

function drain(session) {
  const lines = session.lines;
  session.lines = [];
  return lines;
}

describe('key names', () => {
  it('key names match firmware cli table', () => {
    const expected = new Set(['circle', 'cross', 'triangle', 'square', 'opt', 'touchpad', 'home', 'share',
      'mute', 'l1', 'r1', 'l4', 'r4', 'l3', 'r3', 'up', 'down', 'left', 'right']);
    expect(new Set(KEY_NAMES)).toEqual(expected);
  });

  it('analog trigger and ui combo not exposed', () => {
    for (const bad of ['l2', 'r2', 'ui', 'a', 'zl', 'gl']) {
      expect(() => validateKeys([bad])).toThrow(McpError);
    }
  });
});

describe('validateKeys', () => {
  it('normalizes case and dedupes keeping order', () => {
    expect(validateKeys(['Circle', 'circle', ' R1 '])).toEqual(['circle', 'r1']);
  });

  it('rejects unknown and empty and too many', () => {
    expect(() => validateKeys(['nope'])).toThrow(McpError);
    expect(() => validateKeys([])).toThrow(McpError);
    expect(() => validateKeys(Array.from({ length: MAX_KEYS_PER_CALL + 1 }, (_, i) => String(i)))).toThrow(McpError);
    expect(validateKeys([], { allowEmpty: true })).toEqual([]);
  });
});

describe('KeyStateEngine', () => {
  function makeEngine(pingPeriodS) {
    const engine = new KeyStateEngine(pingPeriodS);
    const conn = new FakeLink();
    const session = new FakeSession(conn);
    return { engine, session, conn };
  }

  it('tap enters state and sends finite command', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.tap(['circle'], 200);
    engine.tick(100.0, session);
    expect(drain(session)).toEqual(['key circle 200']);
  });

  it('tap expiry prunes without release command', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.tap(['circle'], 200);
    engine.tick(100.0, session);
    drain(session);
    engine.tick(100.25, session); // 到期剪枝：固件已自行清掉掩码
    expect(drain(session)).toEqual([]);
    expect(engine.state().held).toEqual([]);
  });

  it('press sends rolling hold', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.0, session);
    expect(drain(session)).toEqual(['key circle 55000']);
  });

  it('second press resends descending remaining', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(101.0, session);
    expect(drain(session)).toEqual(['key circle 54000']);
    engine.press(['cross']);
    engine.tick(101.0, session);
    expect(drain(session)).toEqual(['key cross 55000', 'key circle 54000']);
  });

  it('finite tap joins state when holding', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.0, session);
    drain(session);
    engine.tap(['triangle'], 200);
    engine.tick(100.05, session);
    // 剩余降序：circle 在前，triangle 的 150ms 是最后一条（=最早期限）。
    expect(drain(session)).toEqual(['key circle 54950', 'key triangle 150']);
  });

  it('finite expiry with survivor resends it', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.0, session);
    engine.tap(['triangle'], 200);
    engine.tick(100.05, session);
    drain(session);
    engine.tick(100.3, session); // triangle 到期：固件清了整个掩码，存活键需立刻重发
    expect(drain(session)).toEqual(['key circle 54700']);
  });

  it('subset release sends release then resend', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle', 'cross']);
    engine.tick(100.0, session);
    drain(session);
    engine.release(['circle']);
    engine.tick(100.05, session);
    expect(drain(session)).toEqual(['key release', 'key cross 54950']);
  });

  it('release last key sends only release', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.0, session);
    drain(session);
    engine.release(['circle']);
    engine.tick(100.05, session);
    expect(drain(session)).toEqual(['key release']);
    expect(engine.state().held).toEqual([]);
  });

  it('rolling hold refreshes before expiry', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.0, session);
    drain(session);
    engine.tick(154.6, session); // 余额不足 5s：滚动续期重发
    expect(drain(session)).toEqual(['key circle 55000']);
  });

  it('stick and reset and clear', () => {
    const { engine, session } = makeEngine();
    engine.tick(100.0, session);
    engine.setStick('l', 4095, 2048);
    engine.tick(100.05, session);
    expect(drain(session)).toEqual(['stick l 4095 2048']);
    engine.press(['circle']);
    engine.tick(100.1, session);
    drain(session);
    engine.clear();
    engine.tick(100.15, session);
    expect(drain(session)).toEqual(['key release', 'stick reset']);
    expect(engine.state().held).toEqual([]);
  });

  it('udp link gets ping keepalive', () => {
    const engine = new KeyStateEngine(0.1);
    const conn = fakeUdpLink();
    const session = new FakeSession(conn);
    engine.tick(100.0, session);
    engine.press(['circle']);
    engine.tick(100.05, session);
    engine.tick(100.11, session);
    const decoder = new FrameDecoder();
    let pings = 0;
    for (const blob of conn.written) {
      const { frames } = decoder.feed(blob);
      pings += frames.filter((frame) => frame.type === TYPE_PING).length;
    }
    expect(pings).toBe(2);
  });
});

describe('compileScript', () => {
  it('tap expands to down and up', () => {
    const [events, span] = compileScript([{ t: 10, tap: ['circle'], hold_ms: 100 }], 1, 60000);
    const kinds = events.map(([atMs, _order, kind, payload]) => [atMs, kind, payload]);
    expect(kinds).toEqual([[10.0, 'down', 'circle'], [110.0, 'up', 'circle']]);
    expect(span).toBe(110.0);
  });

  it('same time runs up before down', () => {
    const [events] = compileScript([{ t: 0, up: ['circle'], down: ['cross'] }], 1, 60000);
    expect(events.map(([, _o, kind]) => kind)).toEqual(['up', 'down']);
  });

  it('stick and reset compile', () => {
    const [events] = compileScript([
      { t: 0, stick: { side: 'l', x: 4095, y: 2048 } },
      { t: 50, stick_reset: true },
    ], 1, 60000);
    const payloads = events.filter(([, _o, kind]) => kind === 'stick').map(([, _o, _k, payload]) => payload);
    expect(payloads).toEqual([['l', 4095, 2048]]);
  });

  it('total duration cap', () => {
    expect(() => compileScript([{ t: 1000, tap: ['circle'] }], 100, 60000)).toThrow(McpError);
  });

  it('rejects bad input', () => {
    expect(() => compileScript([], 1, 60000)).toThrow(McpError);
    expect(() => compileScript([{ t: 0, down: ['nope'] }], 1, 60000)).toThrow(McpError);
    expect(() => compileScript([{ t: 0, tap: ['circle'] }], 0, 60000)).toThrow(McpError);
    expect(() => compileScript([{ t: 0, stick: { side: 'x', x: 0, y: 0 } }], 1, 60000)).toThrow(McpError);
  });
});

describe('ReplySink', () => {
  it('noise lines are filtered', async () => {
    const sink = new ReplySink();
    for (const text of ['设备在线（协议 v1）', '已转发 0 帧报告', '反馈 震动 L=off R=off',
      'ok key injected', 'ok keys released', 'ok stick set', 'ok sticks centered']) {
      sink.line(text);
    }
    expect(await sink.collect(sink.mark(), 0.01)).toEqual([]);
  });

  it('secrets masked and lines collected', async () => {
    const sink = new ReplySink();
    const mark = sink.mark();
    sink.line('ok netlog cred ssid=slime_nest pass=hunter2');
    sink.line('ok key injected'); // 注入应答属噪音，不入收集
    expect(await sink.collect(mark, 0.01)).toEqual(['ok netlog cred ssid=slime_nest pass=***']);
    expect(await sink.collect(sink.mark(), 0.01)).toEqual([]); // 收取即消费
  });
});

describe('bridge guards', () => {
  it('device tools refuse before connect', async () => {
    const bridge = new PadBridge(parseCtrlArgs(['--no-pad']), 60000, 600000, 1.0);
    await expect(bridge.query('status')).rejects.toThrow(McpError);
    await expect(bridge.screenshot(null)).rejects.toThrow(McpError);
    await expect(bridge.connect(null, null)).rejects.toThrow(McpError);
    await expect(bridge.connect('COM12', '192.168.1.5')).rejects.toThrow(McpError);
    expect((await bridge.disconnect()).ok).toBe(true); // 未连接时断开是幂等成功
  });
});

describe('tool surface', () => {
  it('tools registered with typed schemas', () => {
    const tools = Object.fromEntries(listTools().map((tool) => [tool.name, tool]));
    const expected = new Set([
      'remapad_connect', 'remapad_disconnect', 'remapad_status', 'remapad_pair', 'remapad_drop',
      'remapad_tap', 'remapad_hold', 'remapad_stick', 'remapad_stick_reset', 'remapad_script',
      'remapad_release_all', 'remapad_screenshot', 'remapad_replay', 'remapad_replay_stop',
    ]);
    expect(new Set(Object.keys(tools))).toEqual(expected);
    const tapSchema = tools.remapad_tap.inputSchema;
    expect(new Set(Object.keys(tapSchema.properties))).toEqual(new Set(['keys', 'hold_ms']));
    expect(tapSchema.properties.keys.type).toBe('array');
    const scriptSchema = tools.remapad_script.inputSchema;
    expect(new Set(Object.keys(scriptSchema.properties))).toEqual(new Set(['actions', 'loop']));
    const connectSchema = tools.remapad_connect.inputSchema;
    expect(new Set(Object.keys(connectSchema.properties))).toEqual(new Set(['port', 'net']));
    expect(new Set(Object.keys(tools.remapad_disconnect.inputSchema.properties ?? {}))).toEqual(new Set());
    // setBridge 面存在：工具执行器经它取桥。
    expect(typeof setBridge).toBe('function');
  });
});
