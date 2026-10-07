// 蓝牙私有触觉流的开关口径与让位门：默认走 0x36，--no-bt-haptics 显式关闭；
// 让位（`haptic audio on`）等私有流真的接到 HD 子帧之后再发。
import { describe, expect, it } from 'vitest';

import { btHapticsWanted, parseCtrlArgs } from '../src/args.js';
import { Session } from '../src/session/session.js';

class FakeLink {
  constructor() {
    this.written = [];
  }

  write(data) {
    this.written.push(Buffer.from(data));
  }

  flush() {}

  purgeInput() {}
}

class FakeHaptics {
  constructor() {
    this.label = '假触觉流';
    this.engaged = false;
  }

  stop() {}
}

/** stats 是方法的形态，对齐蓝牙触觉流 Ds5HapticsBt。 */
class FakeBtHaptics extends FakeHaptics {
  stats() {
    return '写回统计：无写回';
  }
}

function makeSession() {
  const args = parseCtrlArgs([]);
  const link = new FakeLink();
  return [new Session(args, null, link), link];
}

describe('bt haptics switch', () => {
  it('bt private stream is on by default', () => {
    expect(btHapticsWanted(parseCtrlArgs([]))).toBe(true);
  });

  it('no bt haptics opts out', () => {
    expect(btHapticsWanted(parseCtrlArgs(['--no-bt-haptics']))).toBe(false);
  });

  it('explicit on flag still parses', () => {
    expect(btHapticsWanted(parseCtrlArgs(['--bt-haptics']))).toBe(true);
  });
});

describe('haptics yield gate', () => {
  it('yield waits for the stream to engage', () => {
    const [session, link] = makeSession();
    const haptics = new FakeHaptics();
    session.haptics = haptics;
    session.pumpHapticsNotify();
    expect(link.written.some((data) => data.equals(Buffer.from('haptic audio on\r')))).toBe(false);
    haptics.engaged = true;
    session.pumpHapticsNotify();
    expect(link.written.some((data) => data.equals(Buffer.from('haptic audio on\r')))).toBe(true);
    session.pumpHapticsNotify(); // 已让位：不重复发
    expect(link.written.filter((data) => data.equals(Buffer.from('haptic audio on\r')))).toHaveLength(1);
  });

  it('stopHaptics prints stats from method-style haptics without losing this', () => {
    const [session, link] = makeSession();
    session.reporter = { line: (text) => link.written.push(Buffer.from(text)), error: () => {} };
    session.haptics = new FakeBtHaptics();
    session.stopHaptics();
    expect(session.haptics).toBe(null);
    expect(link.written.some((data) => data.equals(Buffer.from('写回统计：无写回')))).toBe(true);
  });
});
