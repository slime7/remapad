// pad_replay.mjs 的落点行为：喇叭路由预置、0x31 的分带与感知曲线、私有流被拒
// 时的回落、以及连接方式按 bus_type 判定（0x0DF2 同时是有线 Edge 的 PID）。
import { crc32 } from 'node:zlib';

import { describe, expect, it, vi } from 'vitest';

import { BT36_SPEAKER_BYTES } from '../src/haptics/wire.js';
import * as padReplay from './samples/pad_replay.mjs';

function key({ lfFreq = 0, lfGain = 0, hfFreq = 0, hfGain = 0 } = {}) {
  return { lf_freq: lfFreq, lf_gain: lfGain, hf_freq: hfFreq, hf_gain: hfGain };
}

function side(props = {}) {
  return [key(props), key(props), key(props)];
}

class FakeDevice {
  constructor(failAfter = null) {
    this.writes = [];
    this.failAfter = failAfter;
    this.closed = false;
  }

  write(report) {
    if (this.failAfter !== null && this.writes.length >= this.failAfter) {
      throw new Error('写回被拒');
    }
    this.writes.push(Buffer.from(report));
  }

  close() {
    this.closed = true;
  }
}

class FakeEncoder {
  encode() {
    return Buffer.alloc(BT36_SPEAKER_BYTES);
  }
}

class SimStub {
  /** 按固定内容回放的替身：keys 左右各一条子帧序列，speaker 给固定音色。 */
  constructor({ left = null, right = null, speaker = [0, 0] } = {}) {
    this.left = left ?? side();
    this.right = right ?? side();
    this.speaker = speaker;
  }

  count() {
    /** 主机声明的子帧数：桩固定按满 3 槽。 */
    return 3;
  }

  at() {
    return [{ 0: this.left, 1: this.right }, this.speaker];
  }
}

function restoreDeps(saved) {
  Object.assign(padReplay.deps, saved);
}

describe('speaker route', () => {
  it('setup reports carry route and volume', () => {
    // 蓝牙与有线两份预置报告：输出路径位段 = 手柄喇叭（0x30）、前级 0x02、
    // 音量档 100、更新使能位都置上。
    const bt = padReplay.buildSetup0x31(0);
    expect(bt).toHaveLength(78);
    expect(bt[0]).toBe(0x31);
    expect(bt[1]).toBe(0x00);
    expect(bt[2]).toBe(0x10);
    expect(bt[3]).toBe(0xa3);
    expect(bt[4]).toBe(0x90);
    expect(bt[8]).toBe(100);
    expect(bt[10]).toBe(0x30);
    expect(bt[40]).toBe(0x02);
    // 尾部 CRC：种子 0xA2 先过、覆盖前 74 字节、小端落位。
    const want = crc32(Buffer.concat([Buffer.from([0xa2]), bt.subarray(0, 74)])) >>> 0;
    expect(bt.readUInt32LE(74)).toBe(want);

    const usb = padReplay.buildSetup0x02();
    expect(usb).toHaveLength(48);
    expect(usb[0]).toBe(0x02);
    expect([usb[1], usb[2]]).toEqual([0xa3, 0x90]);
    expect([usb[6], usb[8], usb[38]]).toEqual([100, 0x30, 0x02]);
  });

  it('bt36 landing primes the route before the stream', async () => {
    // 0x36 落点先发一份 0x31 路由报告，再按节拍发 0x36；配置包的麦克风位保持清零。
    const dev = new FakeDevice();
    await padReplay.runBt36(dev, new SimStub({ speaker: [880, 255] }), 450.0, 20.0, new FakeEncoder());
    expect(dev.writes.length).toBeGreaterThanOrEqual(3);
    expect(dev.writes[0][0]).toBe(0x31);
    expect(dev.writes[0][10]).toBe(0x30);
    expect(dev.writes[1][0]).toBe(0x36);
    expect(dev.writes[1][4]).toBe(0xfe);
    expect(dev.writes[2][0]).toBe(0x36);
  });

  it('private stream rejection falls back to hid', async () => {
    // 私有流写回被拒：提示并回落 0x31 HID 震动，整次回放不崩。
    const fakeDev = new FakeDevice();
    const saved = { ...padReplay.deps };
    let hidRuns = 0;
    try {
      padReplay.deps.findPad = (conn) => (conn === 'bt' ? {} : null);
      padReplay.deps.openHid = () => fakeDev;
      padReplay.deps.runBt36 = () => {
        throw new Error('链路不接受');
      };
      padReplay.deps.runBtHid = async () => {
        hidRuns += 1;
      };
      padReplay.deps.opusAvailable = () => true;
      const log = vi.spyOn(console, 'log').mockImplementation(() => {});
      const code = await padReplay.main(['ns2-search-page.capture', '--pad', 'bt36']);
      const out = log.mock.calls.map((call) => call.join(' ')).join('\n');
      expect(out).toContain('回落 0x31 HID 震动');
      expect(hidRuns).toBe(1);
      expect(code).toBe(0);
    } finally {
      restoreDeps(saved);
      vi.restoreAllMocks();
    }
  });

  it('find pad uses bus type not pid', () => {
    // 连接方式看 bus_type：0x0DF2 也是 DualSense Edge 的有线 PID，按 PID
    // 猜会把直插的 Edge 当成蓝牙。
    const usbEdge = {
      vendorId: 0x054c, productId: 0x0df2, busType: 'usb', usagePage: 0x01, usage: 0x04, path: 'usb-edge',
    };
    const btDs5 = {
      vendorId: 0x054c, productId: 0x0df2, busType: 'bluetooth', usagePage: 0x01, usage: 0x05, path: 'bt-ds5',
    };
    const fakeHid = { devices: () => [usbEdge, btDs5] };
    expect(padReplay.findPad('usb', fakeHid)).toBe(usbEdge);
    expect(padReplay.findPad('bt', fakeHid)).toBe(btDs5);
  });
});

describe('auto landing', () => {
  it('auto prefers usb then the private bt stream', () => {
    const object = {};
    expect(padReplay.resolvePad('auto', object, object, () => true)).toBe('usb');
    expect(padReplay.resolvePad('auto', null, object, () => true)).toBe('bt36');
  });

  it('auto degrades without opus and without a pad', () => {
    expect(padReplay.resolvePad('auto', null, {}, () => false)).toBe('bt32');
    expect(padReplay.resolvePad('auto', null, null, () => true)).toBe('bt');
  });

  it('explicit choice is never overridden', () => {
    for (const choice of ['usb', 'bt', 'bt32', 'bt36', 'bt39']) {
      expect(padReplay.resolvePad(choice, null, null, () => true)).toBe(choice);
    }
  });

  it('auto run lands on the private stream', async () => {
    // auto 在蓝牙上直接开私有流：缺 Opus 也走 0x32，而不是回 0x31。
    const dev = new FakeDevice();
    const saved = { ...padReplay.deps };
    let bt32Runs = 0;
    let hidRuns = 0;
    try {
      padReplay.deps.findPad = (conn) => (conn === 'bt' ? {} : null);
      padReplay.deps.openHid = () => dev;
      padReplay.deps.opusAvailable = () => false;
      padReplay.deps.runBt32 = async () => {
        bt32Runs += 1;
      };
      padReplay.deps.runBtHid = async () => {
        hidRuns += 1;
      };
      const log = vi.spyOn(console, 'log').mockImplementation(() => {});
      await padReplay.main(['ns2-search-page.capture']);
      const out = log.mock.calls.map((call) => call.join(' ')).join('\n');
      expect(out).toContain('auto 落点：bt32');
      expect(bt32Runs).toBe(1);
      expect(hidRuns).toBe(0);
      expect(dev.closed).toBe(true);
    } finally {
      restoreDeps(saved);
      vi.restoreAllMocks();
    }
  });
});

describe('bt hid landing', () => {
  it('motors follow bands with the perceived curve', async () => {
    // 0x31 落点与固件布局行等价：左大马达跟低频带、右小马达跟高频带，
    // 振幅过同一条感知曲线。
    expect(padReplay.perceivedAmp(0)).toBe(0);
    expect(padReplay.perceivedAmp(255)).toBe(255);
    expect(padReplay.perceivedAmp(1)).toBeGreaterThanOrEqual(53);
    expect(padReplay.perceivedAmp(64)).toBe(148);

    const dev = new FakeDevice();
    const sim = new SimStub({ left: side({ lfGain: 255 }), right: side({ hfGain: 64 }) });
    await padReplay.runBtHid(dev, sim, 10.0, 10.0);
    const first = dev.writes[0];
    expect(first[6]).toBe(255); // 左大马达：低频带
    expect(first[5]).toBe(148); // 右小马达：高频带（过了感知曲线）
    expect(first[10]).toBe(0x30); // 预置一并带上：喇叭路由与音量档
  });
});

describe('hd gain', () => {
  function rumblePayload(lfAmp10bit) {
    // 42 字节的 rumble 形态：占位字节 + 左右各 16 字节参数包。
    const frame = Buffer.alloc(5);
    frame.writeUIntLE(lfAmp10bit << 10, 0, 5);
    const params = Buffer.concat([Buffer.from([0x10]), frame, Buffer.alloc(10)]); // 状态字声明 1 个有效子帧
    return Buffer.concat([Buffer.from([0x00]), params, params, Buffer.alloc(9)]);
  }

  it('scaled key clips at the scale top', () => {
    const out = padReplay.scaledKey(key({ lfFreq: 55, lfGain: 5, hfFreq: 190, hfGain: 200 }), 4.0);
    expect([out.lf_gain, out.hf_gain]).toEqual([20, 255]);
    expect([out.lf_freq, out.hf_freq]).toEqual([55, 190]);
  });

  it('sim applies the gain only when asked', () => {
    const payload = rumblePayload(20); // HD 增益 = 20 >> 2 = 5
    const [plain] = new padReplay.FeedbackSim([[0.0, 'rumble', payload]]).at(10.0);
    expect(plain[0][0].lf_gain).toBe(5);
    const [boosted] = new padReplay.FeedbackSim([[0.0, 'rumble', payload]], 4.0).at(10.0);
    expect(boosted[0][0].lf_gain).toBe(20);
  });

  it('sim follows the declared subframe count', () => {
    // 回放与固件同一套轮播语义：轮播长度 = 主机声明的子帧数。
    const carrier = rumblePayload(20); // 状态字 0x10 = 声明 1
    let sim = new padReplay.FeedbackSim([[0.0, 'rumble', carrier]]);
    sim.at(10.0);
    expect([sim.count(0), sim.count(1)]).toEqual([1, 1]);

    const frame = Buffer.alloc(5);
    frame.writeUIntLE(20 << 10, 0, 5);
    const params = Buffer.concat([Buffer.from([0x30]), frame, Buffer.alloc(10)]); // 状态字 0x30 = 声明 3
    sim = new padReplay.FeedbackSim([[
      0.0, 'rumble', Buffer.concat([Buffer.from([0x00]), params, params, Buffer.alloc(9)]),
    ]]);
    sim.at(10.0);
    expect([sim.count(0), sim.count(1)]).toEqual([3, 3]);
  });
});
