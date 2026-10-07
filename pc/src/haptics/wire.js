// 蓝牙私有触觉流的报文形态：0x32（SAxense 逆向）、0x36（DS5Dongle/vds 逆向的
// 触觉+喇叭形态）与 0x39（成对音频+触觉流），加配套的整拍喇叭合成。
// 报文布局与 CRC 规则见 docs/controller-ps.md。

import { crc32 } from 'node:zlib';

import {
  AMP_PEAK_BT,
  AMP_PEAK_USB,
  VoiceState,
  renderKeys,
  renderSpeaker,
  sliceSamples,
  toS8,
} from './synth.js';

/** packet 0x12 承载 64 字节 PCM = 32 帧 × 2 声道 × 8-bit，3000Hz。 */
export const BT_FRAMES = 32;
export const BT_PCM_BYTES = 64;
export const BT_RATE = 3000;
/** 蓝牙私有触觉流（SAxense 逆向）的报文形态：Report ID 0x32、共 142 字节。 */
export const BT_REPORT_LEN = 142;
export const BT_REPORT_ID = 0x32;
/** 发送节拍 = 一报承载的 PCM 时长：32 帧 / 3000Hz ≈ 10.67ms（约 94 报/秒）。 */
export const BT_INTERVAL_S = BT_FRAMES / BT_RATE;
/** CRC32 种子字节（PS 输出报告的 hidp 传输头，与 0x31 同一规则）。 */
export const BT_CRC_SEED = 0xa2;

/** 把 64 字节 PCM（32 帧交错双声道 s8）装进 0x32 私有报告。
 *
 * 142 字节报文：包头 + packet 0x11 的配置与逐报递增序号 + packet 0x12 承载
 * 64 字节 PCM + 补零 + CRC32（种子 0xA2、小端，覆盖前 138 字节）。
 */
export function btBuildReport(pcm, seq) {
  if (pcm.length !== BT_PCM_BYTES) {
    throw new RangeError(`pcm 需要 ${BT_PCM_BYTES} 字节，收到 ${pcm.length}`);
  }
  const report = Buffer.alloc(BT_REPORT_LEN);
  report[0] = BT_REPORT_ID;
  report[2] = 0x11 | 0x80; // packet 0x11，sized 位
  report[3] = 0x07;
  report[4] = 0xfe;
  report[9] = 0xff;
  report[10] = seq & 0xff;
  report[11] = 0x12 | 0x80; // packet 0x12，sized 位
  report[12] = BT_PCM_BYTES;
  pcm.copy(report, 13);
  const crc = crc32(Buffer.concat([Buffer.from([BT_CRC_SEED]), report.subarray(0, BT_REPORT_LEN - 4)])) >>> 0;
  report.writeUInt32LE(crc, BT_REPORT_LEN - 4);
  return report;
}

/** 子帧序列 → 一块 32 帧的触觉 PCM（交错左/右音圈 s8）：蓝牙上没有扬声器
 * 通道，发声段折进两侧音圈——与 USB 直插的音圈行为一致。两侧各过一道音圈
 * 包络门（起音/收音插值）。frames 是这一块的帧数：成对形态（0x39）一报两块，
 * 传 2 × BT_FRAMES。 */
export function btRenderPcm(leftV, rightV, speaker, state, frames = BT_FRAMES) {
  const peak = AMP_PEAK_BT;
  const left = renderKeys(leftV, state.keyPhase[0], state.cursor[0], frames, BT_RATE, peak, sliceSamples(BT_RATE),
    state.gates[0]);
  const right = renderKeys(rightV, state.keyPhase[1], state.cursor[1], frames, BT_RATE, peak, sliceSamples(BT_RATE),
    state.gates[1]);
  const sp = renderSpeaker(speaker[0] ?? [0, 0], state, frames, BT_RATE, peak);
  const out = Buffer.alloc(frames * 2);
  for (let i = 0; i < frames; i++) {
    out[i * 2] = toS8(left[i] + sp[i]) & 0xff;
    out[i * 2 + 1] = toS8(right[i] + sp[i]) & 0xff;
  }
  return out;
}

/** 蓝牙触觉+喇叭流（DS5Dongle/vds 逆向，DualSenseClient 同源）的报文形态：
 * Report ID 0x36、共 398 字节 = 报文头 + 配置包 + 63 字节状态块 +
 * 64 字节触觉 PCM（与 0x32 同格式）+ 200 字节 Opus 喇叭块 + 50 字节保留 +
 * 4 字节 CRC32。 */
export const BT36_REPORT_LEN = 398;
export const BT36_REPORT_ID = 0x36;
/** 喇叭块：Opus CBR 160kbit → 每帧 200 字节；48kHz 声明下帧长 480 样本。 */
export const BT36_SPEAKER_RATE = 48000;
export const BT36_SPEAKER_FRAMES = 480;
export const BT36_SPEAKER_BYTES = 200;
/** 喇叭块的合成时钟：手柄按「一块对一拍」消耗 PCM，480 样本铺满整个
 * BT_INTERVAL_S 节拍（约 45kHz 在播），因此一帧必须装下整拍内容。 */
export const BT36_SPEAKER_BEAT_RATE = Math.round(BT36_SPEAKER_FRAMES / BT_INTERVAL_S);
/** 0x36 的状态块（vds kInitialSetStateData 经 set_audio_out_stream_active
 * 改写后的形态 + 16 字节保留零）：喇叭音量 100（PS5 缺省档）、音频控制字节
 * 的输出路径位段钉在手柄喇叭（0x30，初始 0x09 是耳机/自动）、触觉走音频块；
 * 玩家灯与灯条字节清零——灯归 0x31 写回管。 */
export const BT36_STATE = Buffer.from([
  0xfd, 0xf7, 0x00, 0x00, 0x7f, 100, 0x08, 0x39, 0x00, 0x0f,
  ...new Array(27).fill(0),
  0x01, 0x07, 0x00, 0x00, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
  ...new Array(16).fill(0),
]);
/** 喇叭静默多少秒后从 0x36 退回 0x32：发声段之间的短停顿不切换承载。 */
export const BT36_SPEAKER_TAIL_S = 0.3;
/** 触觉静默多少秒后整条私有流停发：常驻空包会和同频段设备互相干扰。 */
export const BT_HAPTIC_TAIL_S = 0.15;

/** 装一份 0x36 报告：配置包 0x11 + 状态块 0x10 + 触觉 PCM 0x12 + Opus 喇叭块
 * 0x13；reportSeq 是报告序号高半字节、packetSeq 是配置包内滚动序号（公开实现
 * 的同形布局：397 字节声明、[2]=0x91/[11]=0x90/[76]=0x92/[142]=0x93，
 * [344:394] 保留零）。配置包第 4 字节取 0xFE（bit0 = 麦克风采集/双工模式，
 * 置位后手柄会把麦克风音频塞回 0x31 输入报告，被 Windows 与 Steam 当成摇杆
 * 满偏——我们只要喇叭）。CRC32 与 0x31/0x32 同一条规则。 */
export function bt36BuildReport(pcm, speaker, reportSeq, packetSeq) {
  if (pcm.length !== BT_PCM_BYTES) {
    throw new RangeError(`pcm 需要 ${BT_PCM_BYTES} 字节，收到 ${pcm.length}`);
  }
  if (speaker.length !== BT36_SPEAKER_BYTES) {
    throw new RangeError(`speaker 需要 ${BT36_SPEAKER_BYTES} 字节，收到 ${speaker.length}`);
  }
  const report = Buffer.alloc(BT36_REPORT_LEN);
  report[0] = BT36_REPORT_ID;
  report[1] = (reportSeq & 0xf) << 4;
  report[2] = 0x11 | 0x80;
  report[3] = 7;
  report[4] = 0xfe; // 音频段全开、不开麦克风采集（0xFF 会打开双工幻输入）
  report.fill(64, 5, 10); // 音频缓冲长度
  report[10] = packetSeq & 0xff;
  report[11] = 0x10 | 0x80; // 状态块
  report[12] = BT36_STATE.length;
  BT36_STATE.copy(report, 13);
  report[76] = 0x12 | 0x80; // 触觉 PCM
  report[77] = BT_PCM_BYTES;
  pcm.copy(report, 78);
  report[142] = 0x13 | 0x80; // 目标 = 手柄喇叭（0x16 是耳机）
  report[143] = BT36_SPEAKER_BYTES;
  speaker.copy(report, 144);
  const crc = crc32(Buffer.concat([Buffer.from([BT_CRC_SEED]), report.subarray(0, BT36_REPORT_LEN - 4)])) >>> 0;
  report.writeUInt32LE(crc, BT36_REPORT_LEN - 4);
  return report;
}

/** 发声段音色 → 一块整节拍（480 样本 × 立体声 int16 小端）：0x36 的喇叭块
 * 输入。按节拍时钟（BT36_SPEAKER_BEAT_RATE）合成，一块装下整拍的实时内容；
 * 相位与包络挂在 state.speakerBeat 上，与 3kHz 音圈通路互不干扰。 */
export function renderSpeakerBeat(tone, state) {
  const sp = renderSpeaker(tone, state, BT36_SPEAKER_FRAMES, BT36_SPEAKER_BEAT_RATE, AMP_PEAK_USB, true);
  const out = Buffer.alloc(BT36_SPEAKER_FRAMES * 4);
  for (let i = 0; i < sp.length; i++) {
    out.writeInt16LE(sp[i], i * 4);
    out.writeInt16LE(sp[i], i * 4 + 2);
  }
  return out;
}

/** 蓝牙「成对」音频+触觉流（547 字节）：一报带 2 个触觉块与 2 个 Opus 喇叭帧、
 * 节拍 21.33ms，多带的那一块是链路抖动的水垫；没有状态块，音频路由由会话
 * 开始时那一份 0x31 预置保持。 */
export const BT39_REPORT_LEN = 547;
export const BT39_REPORT_ID = 0x39;
export const BT39_HAPTIC_BYTES = BT_PCM_BYTES * 2;
export const BT39_SPEAKER_BYTES = BT36_SPEAKER_BYTES * 2;
export const BT39_INTERVAL_S = BT_INTERVAL_S * 2.0;

/** 装一份 0x39 成对报告：配置包 0x11（长度 6）+ 触觉包 0x12（2 块 64 字节
 * PCM）+ 喇叭包 0x13（2 个 200 字节 Opus 帧）+ 尾部 CRC32。偏移对齐 DS5Dongle
 * 的 audio_bt_task：[2]=0x91、[10]=0x92、[11]=64、[12:140] 触觉、[140]=0x93、
 * [141]=200、[142:542] 喇叭。 */
export function bt39BuildReport(coil, speaker, reportSeq, packetSeq) {
  if (coil.length !== BT39_HAPTIC_BYTES) {
    throw new RangeError(`触觉 PCM 需要 ${BT39_HAPTIC_BYTES} 字节，收到 ${coil.length}`);
  }
  if (speaker.length !== BT39_SPEAKER_BYTES) {
    throw new RangeError(`喇叭块需要 ${BT39_SPEAKER_BYTES} 字节，收到 ${speaker.length}`);
  }
  const report = Buffer.alloc(BT39_REPORT_LEN);
  report[0] = BT39_REPORT_ID;
  report[1] = (reportSeq & 0xf) << 4;
  report[2] = 0x11 | 0x80;
  report[3] = 6;
  report[4] = 0xfe; // 音频段全开、不开麦克风采集（0xFF 会打开双工幻输入）
  report.fill(64, 5, 9); // 音频缓冲长度
  report[9] = packetSeq & 0xff;
  report[10] = 0x12 | 0x80;
  report[11] = BT_PCM_BYTES;
  coil.copy(report, 12);
  report[140] = 0x13 | 0x80;
  report[141] = BT36_SPEAKER_BYTES;
  speaker.copy(report, 142);
  const crc = crc32(Buffer.concat([Buffer.from([BT_CRC_SEED]), report.subarray(0, BT39_REPORT_LEN - 4)])) >>> 0;
  report.writeUInt32LE(crc, BT39_REPORT_LEN - 4);
  return report;
}

/** 一报两块喇叭内容：连着合成 2 个 480 样本的帧（21.33ms），相位与包络跨帧
 * 连续，按 1920 字节一帧切开、交给同一个 48kHz 声明值的编码器逐帧编码。 */
export function renderSpeakerPair(tone, state) {
  const first = renderSpeakerBeat(tone, state);
  const second = renderSpeakerBeat(tone, state);
  return Buffer.concat([first, second]);
}

export { VoiceState };
