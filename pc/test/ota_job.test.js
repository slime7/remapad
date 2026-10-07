// 固件升级（OTA）发送端状态机：串口与 WiFi 共用同一套，按「链路会丢包」设计。
import { describe, expect, it } from 'vitest';

import {
  FrameDecoder,
  TYPE_OTA_BEGIN,
  TYPE_OTA_DATA,
  TYPE_OTA_END,
} from '../src/link/frame.js';
import { UdpLink } from '../src/link/net.js';
import {
  OTA_CODE_TIMEOUT, OTA_STATE_DONE, OTA_STATE_FAILED, OTA_STATE_RECEIVING, OtaJob, makeOtaAck,
} from '../src/session/ota.js';
import { now } from '../src/util.js';

/** 1000 字节镜像 = 5 个数据帧，一窗（16 帧）装得下。 */
const IMAGE = Buffer.from(Array.from({ length: 1000 }, (_, index) => (index * 7) % 256));

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

describe('UdpLink datagram chunking', () => {
  function fakeUdp() {
    const udp = Object.create(UdpLink.prototype);
    udp._closed = false;
    udp._error = null;
    udp.sent = [];
    udp._socket = {
      send: (data, cb) => {
        udp.sent.push(Buffer.from(data));
        cb();
      },
    };
    return udp;
  }

  it('large write splits into datagrams', () => {
    const udp = fakeUdp();
    const payload = Buffer.from(Array.from({ length: 256 }, (_, i) => i));
    const body = Buffer.concat(Array.from({ length: 12 }, () => payload)); // 3072 字节（OTA 整窗的量级）
    udp.write(body);
    expect(Buffer.concat(udp.sent)).toEqual(body);
    expect(udp.sent.map((piece) => piece.length)).toEqual([1024, 1024, 1024]);
  });

  it('small write stays one datagram', () => {
    const udp = fakeUdp();
    udp.write(Buffer.from('status\r'));
    expect(udp.sent).toEqual([Buffer.from('status\r')]);
  });
});

describe('OtaJob', () => {
  function makeJob() {
    const link = new FakeLink();
    const reporter = new RecordingReporter();
    const job = new OtaJob(IMAGE, 'v-test', (data) => link.write(data), reporter);
    return { link, reporter, job };
  }

  function frames(link) {
    return new FrameDecoder().feed(link.written).frames;
  }

  function frameTypes(link) {
    return frames(link).map((frame) => frame.type);
  }

  function dataFrames(link) {
    return frames(link).filter((frame) => frame.type === TYPE_OTA_DATA);
  }

  it('happy path is begin window end', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    expect(frameTypes(link)).toEqual([TYPE_OTA_BEGIN]);
    link.written = Buffer.alloc(0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0, 'old'));
    const data = dataFrames(link);
    expect(data).toHaveLength(5);
    expect(data[data.length - 1].slot).toBe(1); // 末帧带窗口末标记
    link.written = Buffer.alloc(0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 5, 1000));
    expect(frameTypes(link)).toEqual([TYPE_OTA_END]);
    job.onAck(makeOtaAck(OTA_STATE_DONE, 0, 5, 1000));
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(0);
  });

  it('begin resends periodically until ack', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    link.written = Buffer.alloc(0);
    job.tick(1.0); // 重发间隔内静默
    expect(frames(link)).toEqual([]);
    job.tick(2.5); // BEGIN_RETRY_S 到：重发（设备端同尺寸幂等应答）
    expect(frameTypes(link)).toEqual([TYPE_OTA_BEGIN]);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    expect(dataFrames(link)).toHaveLength(5);
  });

  it('begin total timeout fails', () => {
    const { reporter, job } = makeJob();
    job.start(0.0);
    job.tick(OtaJob.BEGIN_ACK_TIMEOUT_S + 1.0);
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
    expect(reporter.errors.length).toBeGreaterThan(0);
  });

  it('window loss resends whole window then resumes', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    link.written = Buffer.alloc(0);
    job.deadline = 0.0; // 强制窗口应答超时
    job.tick(now());
    const resent = dataFrames(link);
    expect(resent).toHaveLength(5);
    expect(resent[0].payload.subarray(0, 2)).toEqual(Buffer.from([0, 0]));
    // 设备只认到第 2 帧：ACK 的 nextSeq 是续传起点
    link.written = Buffer.alloc(0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 2, 400));
    const resumed = dataFrames(link);
    expect(resumed).toHaveLength(3);
    expect(resumed[0].payload.subarray(0, 2)).toEqual(Buffer.from([2, 0]));
  });

  it('stale ack does not rollback progress', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    link.written = Buffer.alloc(0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 2, 400));
    expect(job.confirmed).toBe(400);
    expect(dataFrames(link)).toHaveLength(3);
    link.written = Buffer.alloc(0);
    // 迟到的旧应答（重复 BEGIN 的幂等应答 / 重发窗口的旧 ACK）：不回卷、不重发
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    expect(job.confirmed).toBe(400);
    expect(frames(link)).toEqual([]);
    // 无进展的重复应答（设备写 flash 停顿触发的整窗重发，其序号错误应答）
    // 同样不触发窗口重发——否则与设备的限流应答互相放大。
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 3, 2, 400));
    expect(frames(link)).toEqual([]);
  });

  it('device idle timeout restarts from scratch', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 2, 400));
    // 设备侧 5 秒空闲作废（FAILED + TIMEOUT）：自动重发 BEGIN、进度归零
    job.onAck(makeOtaAck(OTA_STATE_FAILED, OTA_CODE_TIMEOUT, 0, 0));
    expect(job.finished).toBe(false);
    expect(job.phase).toBe('begin');
    expect(job.confirmed).toBe(0);
    expect(frameTypes(link).at(-1)).toBe(TYPE_OTA_BEGIN);
    // 重来有上限：到次数后同样的超时改为终止
    for (let i = 0; i < OtaJob.MAX_SESSION_RESTARTS; i++) {
      job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
      job.onAck(makeOtaAck(OTA_STATE_FAILED, OTA_CODE_TIMEOUT, 0, 0));
    }
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    job.onAck(makeOtaAck(OTA_STATE_FAILED, OTA_CODE_TIMEOUT, 0, 0));
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
  });

  it('device hard failure terminates', () => {
    const { job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    job.onAck(makeOtaAck(OTA_STATE_FAILED, 4, 0, 0)); // FLASH_ERROR
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
  });

  it('end ignores straggler data acks', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 5, 1000));
    link.written = Buffer.alloc(0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 3, 5, 1000));
    expect(job.finished).toBe(false); // 迟到应答被忽略，仍在等 DONE
    job.onAck(makeOtaAck(OTA_STATE_DONE, 0, 5, 1000));
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(0);
  });

  it('end retries until done ack', () => {
    const { link, job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
    job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 5, 1000));
    expect(frameTypes(link).at(-1)).toBe(TYPE_OTA_END);
    link.written = Buffer.alloc(0);
    job.retryDeadline = 0.0; // 强制 END 重发窗口到期（总窗未到）
    job.tick(now() + 1);
    expect(frameTypes(link)).toEqual([TYPE_OTA_END]);
    job.onAck(makeOtaAck(OTA_STATE_DONE, 0, 5, 1000));
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(0);
  });

  it('end total timeout depends on lossy', () => {
    for (const [lossy, expected] of [[false, 1], [true, 0]]) {
      const reporter = new RecordingReporter();
      const job = new OtaJob(IMAGE, 'v-test', () => {}, reporter, lossy);
      job.start(0.0);
      job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 0, 0));
      job.onAck(makeOtaAck(OTA_STATE_RECEIVING, 0, 5, 1000));
      job.deadline = 0.0; // 强制 END 总窗超时（无任何应答）
      job.tick(now());
      expect(job.finished).toBe(true);
      expect(job.exitCode).toBe(expected);
      if (lossy) {
        expect(reporter.errors).toEqual([]); // 不确定完成不按错误刷屏
      }
    }
  });

  it('begin refusal fails the job', () => {
    const { job } = makeJob();
    job.start(0.0);
    job.onAck(makeOtaAck(OTA_STATE_FAILED, 1, 0, 0)); // BUSY
    expect(job.finished).toBe(true);
    expect(job.exitCode).toBe(1);
  });
});
