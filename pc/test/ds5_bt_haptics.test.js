// DS5 蓝牙私有触觉流（0x32/0x36/0x39 报告）的编码与发送：报文布局、CRC32、
// 序号与渲染声道的黄金断言。参考 SAxense.c（公开的互通实现）、SDL 的
// Switch 2 驱动与 Linux hid-playstation.c 的 CRC 规则；字节在这里钉死。
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { sleep } from '../src/util.js';

import { describe, expect, it } from 'vitest';

import { Bt36OpusEncoder } from '../src/haptics/encoder.js';
import { Ds5HapticsBt } from '../src/haptics/bt.js';
import { VoiceState } from '../src/haptics/synth.js';
import {
  BT36_REPORT_ID,
  BT36_REPORT_LEN,
  BT36_SPEAKER_BEAT_RATE,
  BT36_SPEAKER_BYTES,
  BT36_SPEAKER_FRAMES,
  BT36_SPEAKER_TAIL_S,
  BT36_STATE,
  BT39_INTERVAL_S,
  BT39_REPORT_ID,
  BT39_REPORT_LEN,
  BT_FRAMES,
  BT_INTERVAL_S,
  BT_PCM_BYTES,
  BT_RATE,
  BT_REPORT_ID,
  BT_REPORT_LEN,
  bt36BuildReport,
  btBuildReport,
  btRenderPcm,
  renderSpeakerBeat,
} from '../src/haptics/wire.js';
import { silentSide } from './helpers/haptics.js';

const require = createRequire(import.meta.url);

const SILENT_PCM = Buffer.alloc(BT_PCM_BYTES);

/** 逐位 CRC-32（反射多项式 0xEDB88320、初值/终值异或 0xFFFFFFFF），zlib 之外的独立参照。 */
function referenceCrc32(data) {
  let crc = 0xffffffff;
  for (const byte of data) {
    crc ^= byte;
    for (let i = 0; i < 8; i++) {
      const mask = (crc & 1) * 0xedb88320;
      crc = (crc >>> 1) ^ mask;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

describe('0x32 build report', () => {
  it('report layout golden', () => {
    // 报文逐字段（SAxense.c 的互通布局，共 142 字节）。
    const pcm = Buffer.from(Array.from({ length: 64 }, (_, i) => i));
    const report = btBuildReport(pcm, 0x2a);
    expect(report).toHaveLength(BT_REPORT_LEN);
    expect(report[0]).toBe(0x32);
    expect(report[1]).toBe(0x00); // tag/seq 字节保持 0
    expect(report[2]).toBe(0x91); // packet 0x11 + sized 位
    expect(report[3]).toBe(0x07);
    expect(report[4]).toBe(0xfe);
    expect(report.subarray(5, 9)).toEqual(Buffer.alloc(4));
    expect(report[9]).toBe(0xff);
    expect(report[10]).toBe(0x2a); // 递增序号在 packet 0x11 内
    expect(report[11]).toBe(0x92); // packet 0x12 + sized 位
    expect(report[12]).toBe(0x40);
    expect(report.subarray(13, 77)).toEqual(pcm);
    expect(report.subarray(77, 138)).toEqual(Buffer.alloc(61));
    expect(report.subarray(138, 142)).not.toEqual(Buffer.alloc(4));
  });

  it('crc covers header and payload', () => {
    // 尾部 CRC32：种子字节 0xA2 先过一遍、覆盖除 CRC 外的 138 字节、小端落位。
    const report = Buffer.from(btBuildReport(SILENT_PCM, 1));
    const want = referenceCrc32(Buffer.concat([Buffer.from([0xa2]), report.subarray(0, 138)]));
    const got = report.readUInt32LE(138);
    expect(got).toBe(want);
  });

  it('pcm change changes crc', () => {
    const a = btBuildReport(SILENT_PCM, 0);
    const modified = Buffer.concat([Buffer.from([1]), SILENT_PCM.subarray(1)]);
    const b = btBuildReport(modified, 0);
    expect(a.subarray(138, 142)).not.toEqual(b.subarray(138, 142));
  });

  it('sequence increments and wraps', () => {
    const first = btBuildReport(SILENT_PCM, 0xff);
    const second = btBuildReport(SILENT_PCM, 0x00);
    expect(first[10]).toBe(0xff);
    expect(second[10]).toBe(0x00);
  });

  it('rejects wrong pcm size', () => {
    expect(() => btBuildReport(Buffer.alloc(63), 0)).toThrow(RangeError);
  });
});

describe('bt render pcm', () => {
  it('silence when idle', () => {
    const state = new VoiceState();
    const pcm = btRenderPcm(silentSide(), silentSide(), [], state);
    expect(pcm).toEqual(SILENT_PCM);
  });

  it('tone hits both channels interleaved', () => {
    const state = new VoiceState();
    const side = { count: 3, keys: new Array(3).fill([[55, 255], [0, 0]]) };
    const pcm = btRenderPcm(side, silentSide(), [], state);
    const left = pcm.filter((_, i) => i % 2 === 0);
    const right = pcm.filter((_, i) => i % 2 === 1);
    const signed = (b) => (b > 127 ? b - 256 : b);
    expect(Math.max(...left.map((b) => Math.abs(signed(b))))).toBeGreaterThan(40);
    expect(right).toEqual(Buffer.alloc(BT_FRAMES));
    // s8 承载：任何样本都不得越出 8 位刻度。
    for (const b of pcm) {
      expect(signed(b)).toBeGreaterThanOrEqual(-128);
      expect(signed(b)).toBeLessThanOrEqual(127);
    }
  });

  it('speaker segment reaches the coils', () => {
    // 蓝牙上没有扬声器通道：发声段折进两侧音圈 PCM。
    const state = new VoiceState();
    const pcm = btRenderPcm(silentSide(), silentSide(), [[500, 255]], state);
    expect(pcm).not.toEqual(SILENT_PCM);
    const signed = (b) => (b > 127 ? b - 256 : b);
    const left = pcm.filter((_, i) => i % 2 === 0).map(signed);
    const right = pcm.filter((_, i) => i % 2 === 1).map(signed);
    expect(Math.max(...left.map(Math.abs))).toBeGreaterThan(40);
    expect(Math.max(...right.map(Math.abs))).toBeGreaterThan(40);
  });

  it('speaker matches usb coil scale', () => {
    // 发声段在两条承载通路上行为一致：幅度按各自峰值刻度等比。取进入稳态的窗做比较。
    const speaker = [[500, 255]];
    const rms = (values) => Math.sqrt(values.reduce((acc, v) => acc + v * v, 0) / values.length);

    const state = new VoiceState();
    for (let i = 0; i < 2; i++) {
      btRenderPcm(silentSide(), silentSide(), speaker, state); // 预热越过起音段
    }
    const blocks = [];
    for (let i = 0; i < 3; i++) {
      blocks.push(btRenderPcm(silentSide(), silentSide(), speaker, state));
    }
    const pcm = Buffer.concat(blocks);
    // Buffer.map 会把负值强转回无符号字节，先转普通数组再做符号换算。
    const signed = (b) => (b > 127 ? b - 256 : b);
    const btLeft = Array.from(pcm.filter((_, i) => i % 2 === 0)).map(signed);

    const { Ds5HapticsAudio } = require('../src/haptics/audio.js');
    const audio = new Ds5HapticsAudio();
    audio.setParams({ hd: { l: silentSide(), r: silentSide(), speaker: [500, 255] } });
    const frames = 6144;
    audio.renderBlock(frames); // 预热一窗越过 48kHz 起音段
    const block = audio.renderBlock(frames);
    const usbLeft = [];
    for (let i = 0; i < frames; i++) {
      usbLeft.push(block.readInt16LE(i * 8 + 4));
    }

    const ratio = rms(usbLeft) / rms(btLeft);
    const want = 30000 / 127;
    expect(Math.abs(ratio - want)).toBeLessThanOrEqual(want * 0.02);
  });

  it('keys play in time order', () => {
    // 子帧时间轴在蓝牙流上同样保留：3kHz 下每切片 15 样本。
    const state = new VoiceState();
    const side = { count: 3, keys: [[[55, 255], [0, 0]], [[0, 0], [0, 0]], [[55, 255], [0, 0]]] };
    const pcm = btRenderPcm(side, side, [], state);
    const left = pcm.filter((_, i) => i % 2 === 0);
    const signed = (b) => Math.abs(b > 127 ? b - 256 : b);
    expect(Math.max(...left.slice(0, 15).map(signed))).toBeGreaterThan(30);
    expect(left.slice(15, 30)).toEqual(Buffer.alloc(15)); // 子帧 1：静默切片
    expect(Math.max(...left.slice(30, 45).map(signed))).toBeGreaterThan(30); // 回绕到子帧 2
  });

  it('short burst fades out smoothly instead of hard cut', () => {
    // 短震动的收尾是平滑有界的收音尾：收震后第一块仍有声、两个块内落回静音。
    const state = new VoiceState();
    const burst = { count: 1, keys: [[[55, 255], [0, 0]]] };
    const silent = silentSide();
    const signed = (b) => Math.abs(b > 127 ? b - 256 : b);
    const leftPeak = (block) => Math.max(...block.filter((_, i) => i % 2 === 0).map(signed));

    expect(btRenderPcm(burst, silent, [], state)).not.toEqual(SILENT_PCM);
    const tail = [0, 1, 2].map(() => btRenderPcm(silent, silent, [], state));
    expect(leftPeak(tail[0])).toBeGreaterThan(20); // 收震后仍在收音尾
    expect(leftPeak(tail[1])).toBeGreaterThan(0); // 尾内连续衰减
    expect(leftPeak(tail[2])).toBe(0); // 两个块内落回静音
  });

  it('new burst after silence starts from the first subframe', () => {
    // 整段静默后的新震动从子帧 0 起播：强子帧立刻出去。
    const state = new VoiceState();
    const burst = { count: 3, keys: [[[55, 255], [0, 0]], [[0, 0], [0, 0]], [[0, 0], [0, 0]]] };
    const silent = silentSide();
    btRenderPcm(burst, silent, [], state);
    btRenderPcm(burst, silent, [], state);
    btRenderPcm(silent, silent, [], state);
    btRenderPcm(silent, silent, [], state);
    const pcm = btRenderPcm(burst, silent, [], state);
    const signed = (b) => Math.abs(b > 127 ? b - 256 : b);
    const left = pcm.filter((_, i) => i % 2 === 0).map(signed);
    expect(Math.max(...left.slice(0, 4))).toBeGreaterThan(20);
  });

  it('gate saturates for continuous rumble', () => {
    // 连续震动时包络门饱和在满幅：插值只平滑沿，不压稳态强度。
    const state = new VoiceState();
    const burst = { count: 3, keys: new Array(3).fill([[55, 255], [0, 0]]) };
    let pcm;
    for (let i = 0; i < 3; i++) {
      pcm = btRenderPcm(burst, burst, [], state);
    }
    const signed = (b) => Math.abs(b > 127 ? b - 256 : b);
    const left = pcm.filter((_, i) => i % 2 === 0).map(signed);
    expect(Math.max(...left)).toBeGreaterThan(120);
  });
});

describe('sender loop', () => {
  it('engagement is visible in the log', async () => {
    // 「私有流是否真的在驱动」只有日志能看出来：这一行只在第一次接到内容时打一条。
    const fakeDevice = () => ({
      writes: [],
      async write(report) {
        this.writes.push(report);
        throw new Error('done');
      },
    });
    const makeReporter = () => ({ lines: [], errors: [],
      line(text) { this.lines.push(text); },
      error(text) { this.errors.push(text); } });

    const reporter = makeReporter();
    const device = fakeDevice();
    const sender = new Ds5HapticsBt(device, reporter);
    sender.setParams({ hd: { l: { count: 3, keys: new Array(3).fill([[55, 128], [0, 0]]) },
      r: silentSide(), speaker: [0, 0] } });
    await sender.run();
    expect(reporter.lines).toHaveLength(1);
    expect(reporter.lines[0]).toContain('HD 子帧');
    expect(reporter.lines[0]).toContain('0x32');
    // 同一行带首帧的子帧档位：靠它区分「固件没放大」与「标定值」。
    expect(reporter.lines[0]).toContain('55Hz/128');
    expect(reporter.lines[0]).toContain('静默');

    // 没有 HD 段（老固件/未接入）不冒充「已驱动」。
    reporter.lines.length = 0;
    const sender2 = new Ds5HapticsBt(device, reporter);
    sender2.setParams({ hd: null });
    sender2._stop.set();
    await sender2.run();
    expect(reporter.lines).toEqual([]);
  });

  it('short write is counted and reported', async () => {
    let writes = 0;
    const device = {
      async write(report) {
        writes += 1;
        if (writes >= 2) {
          throw new Error('done');
        }
        return report.length - 1; // 少一个字节 = 这一拍没出去
      },
    };
    const reporter = { lines: [], line(text) { this.lines.push(text); }, error(text) { this.lines.push(text); } };
    const sender = new Ds5HapticsBt(device, reporter);
    sender.setParams({ hd: { l: { count: 1, keys: [[[135, 200], [0, 0]]] }, r: silentSide(), speaker: [0, 0] } });
    await sender.run();
    expect(reporter.lines.some((line) => line.includes('短写'))).toBe(true);
    expect(sender.stats()).toContain('短写 1 份');
  });

  it('padded write is not a short write', async () => {
    let writes = 0;
    const device = {
      async write(_report) {
        writes += 1;
        if (writes >= 2) {
          throw new Error('done');
        }
        return 547; // 补齐到描述符声明的输出报告长度
      },
    };
    const sender = new Ds5HapticsBt(device, null);
    sender.setParams({ hd: { l: { count: 1, keys: [[[135, 200], [0, 0]]] }, r: silentSide(), speaker: [0, 0] } });
    await sender.run();
    expect(sender.stats()).not.toContain('短写');
    expect(sender.stats()).toContain('写回统计：1 份');
  });

  it('sender pushes well formed reports', async () => {
    const device = {
      writes: [],
      async write(report) {
        this.writes.push(report);
        if (this.writes.length >= 3) {
          throw new Error('done');
        }
      },
    };
    const sender = new Ds5HapticsBt(device);
    sender.setParams({ hd: {
      l: { count: 3, keys: new Array(3).fill([[55, 128], [0, 0]]) },
      r: silentSide(),
      speaker: [0, 0],
    } });
    await sender.run(); // 第 3 次写回抛错，循环自行退出
    expect(device.writes).toHaveLength(3);
    for (const [i, report] of device.writes.entries()) {
      expect(report).toHaveLength(BT_REPORT_LEN);
      expect(report[10]).toBe(i);
      const want = referenceCrc32(Buffer.concat([Buffer.from([0xa2]), report.subarray(0, 138)]));
      expect(report.readUInt32LE(138)).toBe(want);
      expect(report.subarray(13, 77)).not.toEqual(SILENT_PCM); // 左侧子帧在震
    }

    // 空闲整流停发：静默期一报不发。
    const sender2 = new Ds5HapticsBt(device);
    sender2.setParams({});
    device.writes.length = 0;
    sender2._stop.set(); // 空闲路径不写回：置停止位让循环退出
    await sender2.run();
    expect(device.writes).toEqual([]);
  });

  it('new content wakes the idle sender', async () => {
    const sender = new Ds5HapticsBt({}, null);
    try {
      sender.setParams({});
      expect(sender._wake.isSet()).toBe(false);
      sender.setParams({ hd: { l: { count: 1, keys: [[[55, 200], [0, 0]]] }, r: silentSide(), speaker: [0, 0] } });
      expect(sender._wake.isSet()).toBe(true);
      sender._wake.clear();
      sender.setParams({ hd: { l: silentSide(), r: silentSide(), speaker: [500, 255] } });
      expect(sender._wake.isSet()).toBe(true); // 发声段（折进音圈）也算内容
    } finally {
      await sender.stop();
    }
  });

  it('write failure notifies the session', async () => {
    const losses = [];
    const sender = new Ds5HapticsBt(
      { async write() { throw new Error('write rejected'); } },
      null,
      { onError: (exc) => losses.push(exc) });
    sender.setParams({ hd: {
      l: { count: 1, keys: [[[55, 128], [0, 0]]] },
      r: silentSide(),
      speaker: [0, 0],
    } });
    await sender.run();
    expect(losses).toHaveLength(1);
    expect(losses[0]).toBeInstanceOf(Error);
  });

  it('reports go out raw saxense length', async () => {
    // 0x32 报文按 SAxense 的 142 字节原始形态直写、绝不填充。
    const device = {
      writes: [],
      async write(report) {
        this.writes.push(report);
        if (this.writes.length >= 2) {
          throw new Error('done');
        }
      },
    };
    const sender = new Ds5HapticsBt(device);
    sender.setParams({ hd: {
      l: { count: 1, keys: [[[135, 128], [0, 0]]] },
      r: silentSide(),
      speaker: [0, 0],
    } });
    await sender.run();
    expect(device.writes).toHaveLength(2);
    for (const report of device.writes) {
      expect(report).toHaveLength(BT_REPORT_LEN);
      expect(report[0]).toBe(0x32);
      const want = referenceCrc32(Buffer.concat([Buffer.from([0xa2]), report.subarray(0, 138)]));
      expect(report.readUInt32LE(138)).toBe(want);
    }

    // 填充入口已删：报文也不会被拉长。
    const mod = await import('../src/haptics/index.js');
    expect('btReportWindowsPad' in mod).toBe(false);
    expect('windowsOutputReportLen' in mod).toBe(false);
  });

  it('s8 conversion saturates instead of wrapping', () => {
    const { toS8 } = require('../src/haptics/synth.js');
    expect(toS8(127)).toBe(127);
    expect(toS8(-128)).toBe(-128);
    expect(toS8(300)).toBe(127);
    expect(toS8(-300)).toBe(-128);
    expect(toS8(-32768)).toBe(-128);
    expect(toS8(0)).toBe(0);
  });
});

/** 假时钟：等到期的等待按它推进，用例瞬间跑完一整段节拍。 */
class FakeClock {
  constructor() {
    this.value = 0.0;
  }

  now() {
    return this.value;
  }

  advance(seconds) {
    this.value += seconds;
  }
}

/** 假时钟驱动的发送循环：等到期的等待把假时钟推到到期时刻，空闲等待推进一个
 * 兜底轮询拍（可经 onIdle 在这期间喂新内容）。 */
class GridSender extends Ds5HapticsBt {
  constructor(device, clock, onIdle = null, options = {}) {
    super(device, options.reporter ?? null, { ...options, clock: () => clock.now() });
    this.clock = clock;
    this.idleRounds = 0;
    this._onIdle = onIdle;
  }

  waitUntilDue(due) {
    this.clock.advance(Math.max(0.0, due - this.clock.now()));
    return this._stop.isSet();
  }

  async waitForContent(timeout) {
    this.idleRounds += 1;
    this.clock.advance(timeout);
    if (this._onIdle !== null) {
      this._onIdle(this.idleRounds);
    }
    if (this._wake.isSet()) {
      this._wake.clear();
    }
  }
}

describe('sender timing', () => {
  const coilParams = () => ({
    hd: { l: { count: 3, keys: new Array(3).fill([[55, 200], [0, 0]]) }, r: silentSide(), speaker: [0, 0] },
  });
  const speakerParams = () => ({ hd: { l: silentSide(), r: silentSide(), speaker: [880, 200] } });

  it('slow writes do not stretch the beat', async () => {
    // 写回慢的链路上节拍不被写回耗时推长：每次写回花 3ms，相邻两报的间隔仍是 10.67ms。
    const clock = new FakeClock();
    const device = {
      stamps: [],
      async write(_report) {
        this.stamps.push(clock.now());
        clock.advance(0.003);
        if (clock.now() > 0.5) {
          throw new Error('done');
        }
      },
    };
    const sender = new GridSender(device, clock);
    sender.setParams(coilParams());
    await sender.run();
    expect(device.stamps.length).toBeGreaterThan(40);
    const gaps = device.stamps.slice(1).map((b, i) => b - device.stamps[i]);
    for (const gap of gaps) {
      expect(Math.abs(gap - BT_INTERVAL_S)).toBeLessThan(1e-9);
    }
  });

  it('pcm frames per second match the carrier rate', async () => {
    // 每秒送出的触觉 PCM 帧数与承载采样率一致（3000 帧/秒）：0x32 与 0x36 两条承载都按一报 32 帧的时长出报。
    const fakeEncoder = { encode: () => Buffer.alloc(BT36_SPEAKER_BYTES) };
    class CountingDevice {
      constructor(clock) {
        this._clock = clock;
        this.stamps = [];
      }

      async write(_report) {
        this.stamps.push(this._clock.now());
        if (this._clock.now() > 0.5) {
          throw new Error('done');
        }
      }
    }
    const lanes = [
      ['0x32', coilParams(), {}],
      ['0x36', speakerParams(), { speakerEncoder: fakeEncoder }],
    ];
    for (const [lane, params, options] of lanes) {
      const clock = new FakeClock();
      const device = new CountingDevice(clock);
      const sender = new GridSender(device, clock, null, options);
      sender.setParams(params);
      await sender.run();
      expect(device.stamps.length).toBeGreaterThan(20);
      const span = device.stamps.at(-1) - device.stamps[0];
      const frames = (device.stamps.length - 1) * BT_FRAMES;
      expect(Math.abs(frames / span - BT_RATE)).toBeLessThanOrEqual(BT_RATE * 0.002);
      void lane;
    }
  });

  it('pair form lands two blocks per report', async () => {
    // 成对形态（0x39）：一报 2 块触觉 + 2 帧喇叭、节拍 21.33ms。
    const fakeEncoder = { encode: () => Buffer.alloc(BT36_SPEAKER_BYTES) };
    const clock = new FakeClock();
    const device = {
      reports: [],
      stamps: [],
      async write(report) {
        this.reports.push(Buffer.from(report));
        this.stamps.push(clock.now());
        if (clock.now() > 0.5) {
          throw new Error('done');
        }
      },
    };
    const sender = new GridSender(device, clock, null, { speakerEncoder: fakeEncoder, pair: true });
    sender.setParams(speakerParams());
    await sender.run();
    expect(device.reports.length).toBeGreaterThan(5);
    const first = device.reports[0];
    expect(first).toHaveLength(BT39_REPORT_LEN);
    expect(first[0]).toBe(BT39_REPORT_ID);
    expect(first[3]).toBe(6); // 配置包长度
    expect(first[10]).toBe(0x12 | 0x80);
    expect(first[11]).toBe(BT_PCM_BYTES);
    expect(first[140]).toBe(0x13 | 0x80);
    expect(first[141]).toBe(BT36_SPEAKER_BYTES);
    const gaps = device.stamps.slice(1).map((b, i) => b - device.stamps[i]);
    for (const gap of gaps) {
      expect(Math.abs(gap - BT39_INTERVAL_S)).toBeLessThan(1e-9);
    }
    expect(sender.stats()).toContain('目标 21.33ms');
  });

  it('idle wake does not backfill the grid', async () => {
    // 空闲唤醒后第一拍立刻发出、第二拍起仍按节拍，不连发补报。
    const clock = new FakeClock();
    const silent = { hd: { l: silentSide(), r: silentSide(), speaker: [0, 0] } };
    const state = { wakeAt: null };
    let senderRef;
    const device = {
      stamps: [],
      async write(report) {
        this.stamps.push(clock.now());
        if (state.wakeAt === null && clock.now() >= 0.2) {
          senderRef.setParams(silent);
        }
        if (state.wakeAt !== null && clock.now() > state.wakeAt + 0.05) {
          throw new Error('done');
        }
        void report;
      },
    };
    const onIdle = () => {
      if (state.wakeAt === null) {
        state.wakeAt = clock.now();
        senderRef.setParams(coilParams());
      }
    };
    const sender = new GridSender(device, clock, onIdle);
    senderRef = sender;
    sender.setParams(coilParams());
    await sender.run();
    expect(sender.idleRounds).toBeGreaterThan(0);
    expect(state.wakeAt).not.toBeNull();
    const gaps = device.stamps.slice(1).map((b, i) => b - device.stamps[i]);
    const idleAt = gaps.findIndex((gap) => gap > 1.5 * BT_INTERVAL_S);
    expect(gaps.length - idleAt).toBeGreaterThan(2); // 唤醒后又跑了几拍
    for (const gap of gaps.slice(idleAt + 1)) {
      expect(Math.abs(gap - BT_INTERVAL_S)).toBeLessThan(1e-9);
    }
  });
});

describe('0x36 build report', () => {
  it('report layout golden', () => {
    const pcm = Buffer.from(Array.from({ length: 64 }, (_, i) => i));
    const speaker = Buffer.alloc(BT36_SPEAKER_BYTES, 0xaa);
    const report = bt36BuildReport(pcm, speaker, 0x5, 0xbc);
    expect(report).toHaveLength(BT36_REPORT_LEN);
    expect(report[0]).toBe(0x36);
    expect(report[1]).toBe(0x50); // 报告序号在高半字节
    expect(report[2]).toBe(0x91); // 配置包 0x11 + sized
    expect(report[3]).toBe(7);
    expect(report[4]).toBe(0xfe); // 音频段全开但不开麦克风采集
    expect(report.subarray(5, 10)).toEqual(Buffer.from([64, 64, 64, 64, 64]));
    expect(report[10]).toBe(0xbc); // 配置包滚动序号
    expect(report[11]).toBe(0x90); // 状态块 0x10 + sized
    expect(report[12]).toBe(63);
    expect(report.subarray(13, 76)).toEqual(BT36_STATE);
    expect(report[76]).toBe(0x92); // 触觉包 0x12 + sized
    expect(report[77]).toBe(64);
    expect(report.subarray(78, 142)).toEqual(pcm);
    expect(report[142]).toBe(0x93); // 手柄喇叭 0x13 + sized
    expect(report[143]).toBe(BT36_SPEAKER_BYTES);
    expect(report.subarray(144, 344)).toEqual(speaker);
    expect(report.subarray(344, 394)).toEqual(Buffer.alloc(50)); // 尾部保留区
  });

  it('crc covers body', () => {
    const pcm = Buffer.alloc(64);
    const speaker = Buffer.alloc(BT36_SPEAKER_BYTES);
    const report = bt36BuildReport(pcm, speaker, 0, 0);
    const want = referenceCrc32(Buffer.concat([Buffer.from([0xa2]), report.subarray(0, report.length - 4)]));
    expect(report.readUInt32LE(report.length - 4)).toBe(want);
  });

  it('state block keeps leds to the state reports', () => {
    expect(BT36_STATE).toHaveLength(63);
    expect(BT36_STATE[0]).toBe(0xfd); // 音频各段使能 + 喇叭音量更新
    expect(BT36_STATE[4]).toBe(0x7f); // 耳机音量缺省
    expect(BT36_STATE[5]).toBe(100); // 喇叭音量 = PS5 缺省档
    expect(BT36_STATE[7]).toBe(0x39); // 输出路径 = 手柄喇叭（0x30 位段）
    expect(BT36_STATE.subarray(43, 47)).toEqual(Buffer.alloc(4)); // 玩家灯与灯条 RGB 全零
  });

  it('sender uses bt36 with speaker encoder', async () => {
    const encoder = {
      chunks: [],
      encode(pcm) {
        this.chunks.push(pcm);
        return Buffer.alloc(BT36_SPEAKER_BYTES, 0x55);
      },
    };
    const device = {
      writes: [],
      async write(report) {
        this.writes.push(report);
        if (this.writes.length >= 3) {
          throw new Error('done');
        }
      },
    };
    const sender = new Ds5HapticsBt(device, null, { speakerEncoder: encoder });
    sender.setParams({ hd: { l: { count: 0, keys: [] }, r: { count: 0, keys: [] }, speaker: [880, 255] } });
    await sender.run();
    expect(device.writes).toHaveLength(3);
    expect(encoder.chunks).toHaveLength(3);
    for (const [i, report] of device.writes.entries()) {
      expect(report).toHaveLength(BT36_REPORT_LEN);
      expect(report[0]).toBe(0x36);
      expect(report[1]).toBe((i & 0xf) << 4);
      expect(report.subarray(144, 344)).toEqual(Buffer.alloc(BT36_SPEAKER_BYTES, 0x55));
    }
  });

  it('speaker encoder unavailable falls back to 0x32', async () => {
    const device = {
      writes: [],
      async write(report) {
        this.writes.push(report);
        throw new Error('done');
      },
    };
    const sender = new Ds5HapticsBt(device, null, { speakerEncoder: null });
    expect(sender.speakerActive).toBe(false);
    sender.setParams({ hd: { l: silentSide(), r: silentSide(), speaker: [880, 255] } });
    await sender.run();
    expect(device.writes).toHaveLength(1);
    expect(device.writes[0][0]).toBe(BT_REPORT_ID);
    expect(device.writes[0].subarray(13, 77)).not.toEqual(SILENT_PCM); // 发声段折进音圈
  });

  it('speaker block carries one full beat', () => {
    // 喇叭块要装下一整拍的内容：用 880Hz 音的过零间距直接量合成时钟。
    const state = new VoiceState();
    const silence = renderSpeakerBeat([0, 0], state);
    expect(silence).toHaveLength(BT36_SPEAKER_FRAMES * 4);
    expect(silence).toEqual(Buffer.alloc(silence.length));
    expect(BT36_SPEAKER_BEAT_RATE).toBe(45000);

    const tone = renderSpeakerBeat([880, 255], state);
    const mono = [];
    for (let i = 0; i < tone.length; i += 4) {
      mono.push(tone.readInt16LE(i));
    }
    expect(mono.some((v) => Math.abs(v) > 10000)).toBe(true);
    const crossings = [];
    for (let i = 0; i < mono.length - 1; i++) {
      if (mono[i] < 0 && mono[i + 1] >= 0) {
        const span = mono[i + 1] - mono[i];
        crossings.push(span ? i - mono[i] / span : i);
      }
    }
    expect(crossings.length).toBeGreaterThan(6);
    const periods = crossings.slice(1).map((b, i) => b - crossings[i]);
    const mean = periods.reduce((a, b) => a + b, 0) / periods.length;
    expect(Math.abs(mean - BT36_SPEAKER_BEAT_RATE / 880.0)).toBeLessThanOrEqual(1.0);
  });

  it('bt36 only while speaker has content', async () => {
    // 0x36 只在喇叭有内容（含收音尾）时上；平时与只有触觉时都走 0x32。
    const fakeEncoder = { encode: () => Buffer.alloc(BT36_SPEAKER_BYTES) };
    const fakeDevice = (limit = 3) => ({
      writes: [],
      async write(report) {
        this.writes.push(report);
        if (this.writes.length >= limit) {
          throw new Error('done');
        }
      },
    });

    // 无内容：整流停发，一报不发。
    let device = fakeDevice();
    let sender = new Ds5HapticsBt(device, null, { speakerEncoder: fakeEncoder });
    sender.setParams({});
    sender._stop.set();
    await sender.run();
    expect(device.writes).toEqual([]);

    // 只有触觉、喇叭静默：仍是 0x32。
    device = fakeDevice();
    sender = new Ds5HapticsBt(device, null, { speakerEncoder: fakeEncoder });
    sender.setParams({
      hd: { l: { count: 1, keys: [[[135, 200], [0, 0]]] }, r: { count: 0, keys: [] }, speaker: [0, 0] },
    });
    await sender.run();
    expect(new Set(device.writes.map((r) => r.length))).toEqual(new Set([BT_REPORT_LEN]));

    // 喇叭有音量：切 0x36，喇叭块与触觉块都在。
    device = fakeDevice();
    sender = new Ds5HapticsBt(device, null, { speakerEncoder: fakeEncoder });
    sender.setParams({ hd: { l: { count: 0, keys: [] }, r: { count: 0, keys: [] }, speaker: [880, 255] } });
    await sender.run();
    expect(new Set(device.writes.map((r) => r.length))).toEqual(new Set([BT36_REPORT_LEN]));
    for (const report of device.writes) {
      expect(report[0]).toBe(BT36_REPORT_ID);
      expect(report[142]).toBe(0x93); // 手柄喇叭 + sized
    }

    // 鸣叫停顿期间（喇叭静默、采样还按着）：过收音尾就整流停发。
    device = fakeDevice(1);
    sender = new Ds5HapticsBt(device, null, { speakerEncoder: fakeEncoder });
    sender.setParams({ hd: { l: { count: 0, keys: [] }, r: { count: 0, keys: [] }, speaker: [880, 255] } });
    await sender.run(); // 响一声（写 1 份后抛错退出），盖上发声时间戳
    await sleep(BT36_SPEAKER_TAIL_S + 0.05);
    sender.setParams({ sample: 0x02, hd: { l: { count: 0, keys: [] }, r: { count: 0, keys: [] }, speaker: [0, 0] } });
    sender._stop.set(); // 过尾长即空闲：置停止位让循环退出
    device.writes.length = 0;
    await sender.run();
    expect(device.writes).toEqual([]);
  });
});

describe('capture replay', () => {
  it('search page capture replays to locate samples', () => {
    // 用 pc/test/samples/ns2-search-page.capture 回放「查找手柄」页：采样流
    // 以约 16 Hz 重发定位呼叫 0x02、收尾 0x00。
    const cap = fileURLToPath(new URL('./samples/ns2-search-page.capture', import.meta.url));
    let total = 0;
    let locate = 0;
    let stop = 0;
    for (const line of readFileSync(cap, 'utf8').split(/\r?\n/)) {
      if (line.startsWith('#') || !line.trim()) {
        continue;
      }
      const match = line.match(/B ((?:[0-9a-f]{2} ?)+)$/);
      expect(match).not.toBeNull();
      const payload = Buffer.from(match[1].replace(/ /g, ''), 'hex');
      total += 1;
      expect(payload[0]).toBe(0x00); // 复合输出的填充字节
      expect(payload.subarray(1, 33)).toEqual(Buffer.alloc(32)); // 震动段全程静置
      const frame = payload.subarray(33);
      expect(frame[0]).toBe(0x0a); // 触觉采样命令
      const sample = frame[8];
      if (sample === 0x02) {
        locate += 1;
      } else if (sample === 0x00) {
        stop += 1;
      }
    }
    expect(total).toBe(223);
    expect(locate).toBe(207);
    expect(stop).toBe(16);
  });
});

/** 发声段一帧经真实 Opus 编解码后音高必须对：合成按节拍时钟（45kHz）写 480
 * 样本、交给声明 48kHz 的编码器，解码回来是 音高×48/45——手柄按「一块对一拍」
 * 播回（≈45kHz）后正好是原音高。 */
describe('speaker pitch round trip', () => {
  function decodeStereo(packet) {
    const { OpusDecoder } = require('audify');
    const decoder = new OpusDecoder(48000, 2);
    const raw = decoder.decode(packet, BT36_SPEAKER_FRAMES);
    const mono = [];
    for (let i = 0; i < raw.length; i += 4) {
      mono.push(raw.readInt16LE(i));
    }
    return mono;
  }

  /** 滑动频率点的幅度谱峰值（避开频域库）：比过零计数抗噪，能分辨 6.25% 的音高差。 */
  function dftPeakHz(mono, rate, lo, hi, skip) {
    const window = mono.slice(skip);
    let bestHz = 0.0;
    let bestPow = -1.0;
    for (let hz = lo; hz <= hi; hz += 1.0) {
      const w = (2 * Math.PI * hz) / rate;
      let re = 0.0;
      let im = 0.0;
      for (let k = 0; k < window.length; k++) {
        re += window[k] * Math.cos(w * k);
        im += window[k] * Math.sin(w * k);
      }
      const power = re * re + im * im;
      if (power > bestPow) {
        bestPow = power;
        bestHz = hz;
      }
    }
    return bestHz;
  }

  it('chirp keeps its pitch through opus', async () => {
    let encoder;
    try {
      encoder = new Bt36OpusEncoder();
    } catch (exc) {
      console.warn(`Opus 编码器不可用，跳过：${exc.message}`);
      return;
    }
    for (const playedHz of [880.0, 1175.0]) {
      const state = new VoiceState();
      const packets = [];
      for (let i = 0; i < 8; i++) {
        packets.push(encoder.encode(renderSpeakerBeat([playedHz, 255], state)));
      }
      expect(new Set(packets.map((p) => p.length))).toEqual(new Set([BT36_SPEAKER_BYTES]));
      let mono = [];
      for (const packet of packets) {
        mono = mono.concat(decodeStereo(packet));
      }
      expect(mono).toHaveLength(BT36_SPEAKER_FRAMES * packets.length);
      const beatHz = (playedHz * 48000) / BT36_SPEAKER_BEAT_RATE;
      // 音高落在「按整拍合成」的位置（48kHz 声明下比原音高高 6.25%）。
      const got = dftPeakHz(mono, 48000, playedHz * 0.9, playedHz * 1.2, 1600);
      expect(Math.abs(got - beatHz)).toBeLessThanOrEqual(beatHz * 0.02);
      expect(Math.abs(got - playedHz)).toBeGreaterThan(Math.abs(got - beatHz));
    }
  });
});
