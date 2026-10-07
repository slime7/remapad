// amiibo 上传的 PC 侧用例：帧载荷编解码、dump 校验与上传状态机。
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

import { afterEach, describe, expect, it } from 'vitest';

import {
  AMIIBO_CODE_BAD_HEADER,
  AMIIBO_CODE_STORE_ERROR,
  AMIIBO_DATA_MAX,
  AMIIBO_NAME_MAX,
  AMIIBO_SIG_SIZE,
  AMIIBO_TAG_SIZE,
  FrameDecoder,
  TYPE_AMIIBO_ACK,
  TYPE_AMIIBO_BEGIN,
  TYPE_AMIIBO_DATA,
  TYPE_AMIIBO_END,
  WIRE_MAX_PAYLOAD,
  amiiboBeginPayload,
  amiiboDataPayload,
  encode,
  parseAmiiboAck,
} from '../src/link/frame.js';
import {
  AMIIBO_STATE_DONE, AMIIBO_STATE_FAILED, AMIIBO_STATE_RECEIVING, AmiiboJob, makeAmiiboAck,
} from '../src/session/amiibo-job.js';
import { AmiiboError, loadAmiibo } from '../src/session/image.js';

function makeDump() {
  return Buffer.from(Array.from({ length: AMIIBO_TAG_SIZE }, (_, index) => index % 256));
}

function makeDumpWithSig() {
  return Buffer.concat([
    makeDump(),
    Buffer.from(Array.from({ length: AMIIBO_SIG_SIZE }, (_, index) => (0xc0 + index) % 256)),
  ]);
}

class FakeLink {
  constructor() {
    this.written = Buffer.alloc(0);
  }

  write(data) {
    this.written = Buffer.concat([this.written, data]);
  }
}

class RecordingReporter {
  constructor() {
    this.lines = [];
    this.errors = [];
    this.events = [];
  }

  line(text) {
    this.lines.push(text);
  }

  error(text) {
    this.errors.push(text);
  }

  event(name, fields) {
    this.events.push([name, fields]);
  }
}

const tempDirs = [];
afterEach(() => {
  while (tempDirs.length) {
    rmSync(tempDirs.pop(), { recursive: true, force: true });
  }
});

function tempFile(name, data) {
  const dir = mkdtempSync(join(tmpdir(), 'remapad-'));
  tempDirs.push(dir);
  const path = join(dir, name);
  writeFileSync(path, data);
  return path;
}

describe('amiibo payloads', () => {
  function decodeOne(frame) {
    const { frames } = new FrameDecoder().feed(frame);
    expect(frames).toHaveLength(1);
    return frames[0];
  }

  it('begin payload carries name and size', () => {
    const frame = decodeOne(encode(TYPE_AMIIBO_BEGIN, 0, 0, amiiboBeginPayload('Alm', 540)));
    expect(frame.type).toBe(TYPE_AMIIBO_BEGIN);
    expect(frame.payload).toEqual(Buffer.concat([
      Buffer.from([3]), Buffer.from('Alm'), Buffer.from([0x1c, 0x02, 0, 0]),
    ]));
  });

  it('begin payload rejects empty and overlong name', () => {
    expect(() => amiiboBeginPayload('', 540)).toThrow();
    expect(() => amiiboBeginPayload('x'.repeat(32), 540)).toThrow();
  });

  it('data payload keeps wire limit', () => {
    const chunk = Buffer.alloc(AMIIBO_DATA_MAX, 0x5a);
    const frame = decodeOne(encode(TYPE_AMIIBO_DATA, 0, 0, amiiboDataPayload(400, chunk), WIRE_MAX_PAYLOAD));
    expect(frame.type).toBe(TYPE_AMIIBO_DATA);
    expect(frame.payload.subarray(0, 2)).toEqual(Buffer.from([0x90, 0x01]));
    expect(frame.payload.subarray(2)).toEqual(chunk);
    expect(() => amiiboDataPayload(0, Buffer.alloc(AMIIBO_DATA_MAX + 1))).toThrow();
  });

  it('ack round trip', () => {
    const ack = Buffer.concat([Buffer.from([2, 0]), Buffer.from([0x1c, 0x02, 0, 0]), Buffer.from([3])]);
    const frame = decodeOne(encode(TYPE_AMIIBO_ACK, 0, 0, ack));
    const parsed = parseAmiiboAck(frame.payload);
    expect(parsed.stateId).toBe(2);
    expect(parsed.codeId).toBe(0);
    expect(parsed.received).toBe(540);
    expect(parsed.slot).toBe(3);
  });
});

describe('loadAmiibo', () => {
  it('rejects wrong size', () => {
    const path = tempFile('Alm.bin', Buffer.alloc(512));
    expect(() => loadAmiibo(path)).toThrow(AmiiboError);
  });

  it('accepts dump with signature', () => {
    // 572 字节（镜像 + 厂商签名）原样上传：签名段进设备读缓冲头区。
    const path = tempFile('Bokoblin.bin', makeDumpWithSig());
    const { name, data } = loadAmiibo(path);
    expect(name).toBe('Bokoblin');
    expect(data).toEqual(makeDumpWithSig());
  });

  it('rejects missing file', () => {
    expect(() => loadAmiibo('Z:/definitely/not/here.bin')).toThrow(AmiiboError);
  });

  it('name comes from stem and stays within limit', () => {
    const path = tempFile('Samus_Aran.bin', makeDump());
    const first = loadAmiibo(path);
    expect(first.name).toBe('Samus_Aran');
    expect(first.data).toEqual(makeDump());

    // 31 字节上限按 UTF-8 截断，不在多字节字符中间留半个字。
    const longName = '火'.repeat(20);
    const longPath = tempFile(`${longName}.bin`, makeDump());
    const second = loadAmiibo(longPath);
    expect(Buffer.byteLength(second.name, 'utf8')).toBeLessThanOrEqual(AMIIBO_NAME_MAX);
    expect(second.name).toBe('火'.repeat(10));
  });
});

describe('AmiiboJob', () => {
  function makeJob() {
    const link = new FakeLink();
    const reporter = new RecordingReporter();
    const job = new AmiiboJob('Alm', makeDump(), (data) => link.write(data), reporter);
    return { link, reporter, job };
  }

  function ack(job, state, code, received, slot = 0xff) {
    job.onAck(makeAmiiboAck(state, code, received, slot));
  }

  it('upload sequence is begin data end', () => {
    const { link, reporter, job } = makeJob();
    job.start(0.0);
    let frames = new FrameDecoder().feed(link.written).frames;
    expect(frames).toHaveLength(1);
    expect(frames[0].type).toBe(TYPE_AMIIBO_BEGIN);
    expect(frames[0].payload).toEqual(amiiboBeginPayload('Alm', 540));

    ack(job, AMIIBO_STATE_RECEIVING, 0, 0);
    frames = new FrameDecoder().feed(link.written).frames;
    const dataFrames = frames.filter((frame) => frame.type === TYPE_AMIIBO_DATA);
    expect(dataFrames.map((frame) => frame.payload.subarray(0, 2))).toEqual([
      Buffer.from([0, 0]), Buffer.from([0xc8, 0x00]), Buffer.from([0x90, 0x01]),
    ]);
    expect(dataFrames.map((frame) => frame.payload.length - 2)).toEqual([200, 200, 140]);
    expect(dataFrames.map((frame) => frame.payload.subarray(2))).toEqual([
      makeDump().subarray(0, 200), makeDump().subarray(200, 400), makeDump().subarray(400, 540),
    ]);

    // 中途的 ACK 不触发重发（三帧已经全在途）。
    link.written = Buffer.alloc(0);
    ack(job, AMIIBO_STATE_RECEIVING, 0, 200);
    expect(new FrameDecoder().feed(link.written).frames).toEqual([]);

    // 收满即发 END；设备回 DONE 后任务收口并带出槽位号。
    ack(job, AMIIBO_STATE_RECEIVING, 0, 540);
    frames = new FrameDecoder().feed(link.written).frames;
    expect(frames.map((frame) => frame.type)).toEqual([TYPE_AMIIBO_END]);
    ack(job, AMIIBO_STATE_DONE, 0, 540, 2);
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(0);
    expect(reporter.lines.join(' ')).toContain('槽位 2');
  });

  it('timeout resends from last confirmed byte', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    ack(job, AMIIBO_STATE_RECEIVING, 0, 0);
    const sentAll = new FrameDecoder().feed(link.written).frames.length;

    job.received = 200;
    job.deadline = 0.0;
    job.tick(100.0);
    const resent = new FrameDecoder().feed(link.written).frames.slice(sentAll);
    expect(resent.map((frame) => frame.payload.subarray(0, 2))).toEqual([
      Buffer.from([0xc8, 0x00]), Buffer.from([0x90, 0x01]),
    ]);
  });

  it('begin refusal fails the job', () => {
    const { reporter, job } = makeJob();
    job.start(0.0);
    ack(job, AMIIBO_STATE_FAILED, AMIIBO_CODE_BAD_HEADER, 0);
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
    expect(reporter.errors.length).toBeGreaterThan(0);
  });

  it('store failure at end fails the job', () => {
    const { job } = makeJob();
    job.start(0.0);
    ack(job, AMIIBO_STATE_RECEIVING, 0, 0);
    ack(job, AMIIBO_STATE_RECEIVING, 0, 540);
    ack(job, AMIIBO_STATE_FAILED, AMIIBO_CODE_STORE_ERROR, 540);
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
  });
});
