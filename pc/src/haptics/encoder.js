// 48kHz 立体声 10ms CBR Opus 编码器（vds SpeakerEncoder 同参数：VBR 关、
// complexity 0、码率 160kbit）。块尾不足 200 字节时补零，超长截断。
// 构造失败（缺 @discordjs/opus 等都按回落处理）由调用方捕获。

import { createRequire } from 'node:module';

import { BT36_SPEAKER_BYTES, BT36_SPEAKER_FRAMES, BT36_SPEAKER_RATE } from './wire.js';

const require = createRequire(import.meta.url);

/** OPUS_SET_BITRATE / OPUS_SET_VBR / OPUS_SET_COMPLEXITY 的 CTL 请求号。 */
const OPUS_SET_BITRATE = 4002;
const OPUS_SET_VBR = 4006;
const OPUS_SET_COMPLEXITY = 4010;

export class Bt36OpusEncoder {
  constructor() {
    const { OpusEncoder } = require('@discordjs/opus');
    this._encoder = new OpusEncoder(BT36_SPEAKER_RATE, 2);
    this._encoder.applyEncoderCTL(OPUS_SET_BITRATE, BT36_SPEAKER_BYTES * 8 * 100);
    this._encoder.applyEncoderCTL(OPUS_SET_VBR, 0);
    this._encoder.applyEncoderCTL(OPUS_SET_COMPLEXITY, 0);
  }

  /** 480 样本 × 立体声 s16 → 一个 200 字节喇叭块。 */
  encode(pcm) {
    if (pcm.length !== BT36_SPEAKER_FRAMES * 4) {
      throw new RangeError(`喇叭 PCM 需要 ${BT36_SPEAKER_FRAMES * 4} 字节，收到 ${pcm.length}`);
    }
    const packet = this._encoder.encode(pcm);
    if (packet.length >= BT36_SPEAKER_BYTES) {
      return packet.subarray(0, BT36_SPEAKER_BYTES);
    }
    return Buffer.concat([packet, Buffer.alloc(BT36_SPEAKER_BYTES - packet.length)]);
  }
}
