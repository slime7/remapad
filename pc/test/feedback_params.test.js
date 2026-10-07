// FEEDBACK 帧载荷解析：两带强度、触觉采样、频率落地值与 HD 时序子帧表。
import { describe, expect, it } from 'vitest';

import { FrameDecoder, TYPE_FEEDBACK, encode, feedbackParams } from '../src/link/frame.js';

function payloadWithFreqs({ lfL = 55, lfR = 60, hfL = 190, hfR = 200 } = {}) {
  const head = Buffer.from([
    1, 1, // 左右使能
    200, 128, // 低频强度 L/R
    0x01, 0x02, // 玩家灯 / 触觉采样
    96, 64, // 高频强度 L/R
  ]);
  const lf = Buffer.alloc(4);
  lf.writeUInt32LE(lfL | (lfR << 16), 0);
  const hf = Buffer.alloc(4);
  hf.writeUInt32LE(hfL | (hfR << 16), 0);
  return Buffer.concat([head, lf, hf]);
}

function payloadWithHd() {
  // 57 字节 HD 版：左 2 个有效子帧、右 1 个、扬声器发声。
  const payload = Buffer.alloc(57);
  payloadWithFreqs().copy(payload, 0);
  payload[16] = 2; // 左有效子帧数
  Buffer.from([55, 0, 100, 190, 0, 64]).copy(payload, 17); // 子帧 0：低 55Hz/100 + 高 190Hz/64
  Buffer.from([90, 0, 2, 0, 0, 0]).copy(payload, 23); // 子帧 1：低 90Hz/2，高频静默
  payload[35] = 1; // 右有效子帧数
  Buffer.from([0xe4, 0x01, 128, 0, 0, 0]).copy(payload, 36); // 子帧 0：低 484Hz/128，高频静默
  payload.writeUInt16LE(880, 54);
  payload[56] = 255;
  return payload;
}

describe('feedbackParams', () => {
  it('full payload carries bands and freqs', () => {
    const params = feedbackParams(payloadWithFreqs());
    expect(params.rumbleOn).toEqual([true, true]);
    expect(params.lfAmp).toEqual([200, 128]);
    expect(params.hfAmp).toEqual([96, 64]);
    expect(params.playerLed).toBe(0x01);
    expect(params.sample).toBe(0x02);
    expect(params.lfFreq).toEqual([55, 60]);
    expect(params.hfFreq).toEqual([190, 200]);
    expect(params.hd).toBeNull();
  });

  it('hd payload carries temporal keys', () => {
    const params = feedbackParams(payloadWithHd());
    const hd = params.hd;
    expect(hd.l.count).toBe(2);
    expect(hd.l.keys).toEqual([[[55, 100], [190, 64]], [[90, 2], [0, 0]]]);
    expect(hd.r.count).toBe(1);
    expect(hd.r.keys).toEqual([[[484, 128], [0, 0]]]);
    expect(hd.speaker).toEqual([880, 255]);
    // 基础段照常解析（打印与老通路共用一份参数）。
    expect(params.lfAmp).toEqual([200, 128]);
  });

  it('hd keys beyond count are dropped', () => {
    const payload = Buffer.from(payloadWithHd());
    payload[16] = 1; // 左侧只声明 1 个有效子帧
    const params = feedbackParams(payload);
    expect(params.hd.l.keys).toEqual([[[55, 100], [190, 64]]]);
  });

  it('legacy payload has no freqs', () => {
    const params = feedbackParams(payloadWithFreqs().subarray(0, 12));
    expect(params.lfAmp).toEqual([200, 128]);
    expect(params.hfAmp).toEqual([96, 64]);
    expect(params.lfFreq).toBeNull();
    expect(params.hfFreq).toBeNull();
    expect(params.hd).toBeNull();
  });

  it('short payload is rejected', () => {
    expect(feedbackParams(Buffer.alloc(7, 0x01))).toBeNull();
    expect(feedbackParams(Buffer.alloc(0))).toBeNull();
  });

  it('round trip through frame decoder', () => {
    const payload = payloadWithFreqs();
    const { frames } = new FrameDecoder().feed(encode(TYPE_FEEDBACK, 0, 3, payload));
    expect(feedbackParams(frames[0].payload).hfFreq).toEqual([190, 200]);
  });

  it('hd payload round trip through frame decoder', () => {
    const payload = payloadWithHd();
    const { frames } = new FrameDecoder().feed(encode(TYPE_FEEDBACK, 0, 3, payload));
    expect(feedbackParams(frames[0].payload).hd.speaker).toEqual([880, 255]);
  });
});
