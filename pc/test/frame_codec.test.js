// 桥接帧编解码：成帧、丢帧重同步与文本旁路（GUI 与命令行共用这一份）。
import { describe, expect, it } from 'vitest';

import {
  DECODE_MAX_PAYLOAD,
  FrameDecoder,
  MAX_PAYLOAD,
  TYPE_DETACH,
  TYPE_FEEDBACK,
  TYPE_OTA_DATA,
  TYPE_PING,
  TYPE_REPORT,
  WIRE_MAX_PAYLOAD,
  encode,
} from '../src/link/frame.js';

function decode(data) {
  return new FrameDecoder().feed(data);
}

describe('FrameDecoder', () => {
  it('round trip', () => {
    const frame = encode(TYPE_REPORT, 0, 5, Buffer.from([0x01, 0x02]));
    const { frames, text } = decode(frame);
    expect(frames).toEqual([{ type: TYPE_REPORT, slot: 0, seq: 5, payload: Buffer.from([0x01, 0x02]) }]);
    expect(text).toEqual(Buffer.alloc(0));
  });

  it('accepts long payloads', () => {
    const payload = Buffer.from(Array.from({ length: 200 }, (_, i) => i));
    const frame = encode(TYPE_OTA_DATA, 0, 3, payload, WIRE_MAX_PAYLOAD);
    const { frames } = decode(frame);
    expect(frames[0].payload).toEqual(payload);
  });

  it('text passes through', () => {
    const { frames, text } = decode(Buffer.from('cli ready\r\n'));
    expect(frames).toEqual([]);
    expect(text).toEqual(Buffer.from('cli ready\r\n'));
  });

  it('garbage before a frame is kept as text', () => {
    const frame = encode(TYPE_PING, 0, 0, Buffer.from([0x01]));
    const { frames, text } = decode(Buffer.concat([Buffer.from([0x11]), frame]));
    expect(text).toEqual(Buffer.from([0x11]));
    expect(frames).toHaveLength(1);
  });

  it('broken crc is not delivered as a frame', () => {
    const frame = Buffer.from(encode(TYPE_REPORT, 0, 5, Buffer.from([0x03])));
    frame[frame.length - 1] ^= 0x5a;
    frame[frame.length - 2] ^= 0x11;
    const { frames, text } = decode(frame);
    expect(frames).toEqual([]);
    // 坏帧退化成文本而不是被静默丢掉；解码器最多留一个疑似帧头的字节等下一批。
    expect(frame.length - text.length).toBeLessThanOrEqual(1);
  });

  it('frame split across reads', () => {
    const frame = encode(TYPE_FEEDBACK, 0, 7, Buffer.from([0x00, 0x01, 0x02]));
    const decoder = new FrameDecoder();
    const first = decoder.feed(frame.subarray(0, 4));
    expect(first.frames).toEqual([]);
    expect(first.text).toEqual(Buffer.alloc(0));
    const second = decoder.feed(frame.subarray(4));
    expect(second.frames).toEqual([{ type: TYPE_FEEDBACK, slot: 0, seq: 7, payload: Buffer.from([0x00, 0x01, 0x02]) }]);
  });

  it('two frames in one read', () => {
    const first = encode(TYPE_PING, 0, 0, Buffer.from([0x01]));
    const second = encode(TYPE_DETACH, 0, 1, Buffer.from([0x02]));
    const { frames, text } = decode(Buffer.concat([first, second]));
    expect(text).toEqual(Buffer.alloc(0));
    expect(frames.map((frame) => frame.type)).toEqual([TYPE_PING, TYPE_DETACH]);
  });

  it('rejects payloads beyond the wire limit at encode time', () => {
    expect(() => encode(TYPE_REPORT, 0, 0, Buffer.alloc(DECODE_MAX_PAYLOAD + 1))).toThrow(RangeError);
  });

  it('carries a full bluetooth DS5 report in one REPORT frame', () => {
    // DS5 蓝牙 0x31 报告 78 字节 + 8 字节设备标识 = 86，恰好顶到报文帧上限。
    const payload = Buffer.alloc(MAX_PAYLOAD, 0x31);
    const frame = encode(TYPE_REPORT, 0, 1, payload);
    const { frames } = decode(frame);
    expect(frames).toEqual([{ type: TYPE_REPORT, slot: 0, seq: 1, payload }]);
    expect(() => encode(TYPE_REPORT, 0, 0, Buffer.alloc(MAX_PAYLOAD + 1))).toThrow(RangeError);
  });
});
