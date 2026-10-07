// 把本目录的 .capture 采集样本回放到 DualSense 手柄。
// 回放引擎镜像固件的转换链（ns2_output 解码 → pad_feedback_hd_render 落地 →
// 采样音色时间线），落点：usb（WASAPI 4ch）/ bt（0x31 双马达）/ bt32（0x32 私有流）/
// bt36（0x36 HD+喇叭）/ bt39（0x39 成对形态）。回放中 Ctrl-C 随时干净退出。
// 用法：node pc/test/samples/pad_replay.mjs ns2-search-page.capture --pad bt36 --speed 0.5
// 数据与核对状态见 docs/controller-ps.md。

import { readdirSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { createRequire } from 'node:module';
import { Command } from 'commander';
import { crc32 } from 'node:zlib';

import {
  BT_FRAMES,
  BT_INTERVAL_S,
  BT39_INTERVAL_S,
  VoiceState,
  bt36BuildReport,
  bt39BuildReport,
  btBuildReport,
  btRenderPcm,
  renderSpeakerBeat,
  renderSpeakerPair,
} from '../../src/haptics/wire.js';
import { Bt36OpusEncoder } from '../../src/haptics/encoder.js';
import { Ds5HapticsAudio } from '../../src/haptics/audio.js';

const require = createRequire(import.meta.url);

export const SAMPLES_DIR = fileURLToPath(new URL('.', import.meta.url));

/** 依赖注入点：测试用例替换这里的条目来模拟缺手柄/写回被拒等场景。 */
export const deps = {
  loadHid: () => require('node-hid'),
  opusAvailable,
  findPad,
  openHid,
  runBt32,
  runBt36,
  runBt39,
  runBtHid,
};

// ---------------------------------------------------------------- 采集解析

const LINE_RE = /\s*\+([\d.]+)s (\w+)\[0x([0-9a-f]+)\] seq=(\d+)\s+(\d+)B\s+(.*)$/;

export function loadCapture(path) {
  const rows = [];
  const text = readFileSync(path, 'utf8');
  for (const line of text.split(/\r?\n/)) {
    if (line.startsWith('#') || !line.trim()) {
      continue;
    }
    const match = LINE_RE.exec(line);
    if (match === null) {
      throw new Error(`看不懂的抓包行：${line}`);
    }
    rows.push([Number(match[1]), match[2], Buffer.from(match[6].trim(), 'hex')]);
  }
  if (!rows.length) {
    throw new Error(`${path} 里没有记录`);
  }
  return rows;
}

// ---------------------------------------------------------------- 固件镜像

const OCT_FRAC = [92682, 77935, 71461, 68442, 66965, 66229, 65878];
const FREQ_MIN = 20;
const FREQ_MAX = 500;
const FREQ_DEFAULT = [80, 135];
/** 采样音色表（与 pad/feedback.c 的 s_haptic_bank 同数据）：段边界 ms → (幅度, 音高)。 */
export const LOCATE_STEPS = [
  [220, 0xc0, 0], [400, 0, 0], [500, 0x80, 880], [600, 0, 0], [700, 0x80, 1175], [1200, 0, 0],
];
export const LF_BEEP_STEPS = [[1000, 0xc0, 0], [1100, 0, 0]];
export const DEFAULT_STEPS = [[120, 0xc0, 0], [300, 0, 0]];
export const HAPTIC_HOLD_MS = 300;

/** 固件 key_freq_hz 的镜像：9 位 log2 频率码 → Hz，码 0 = 未声明。 */
export function keyFreqHz(code) {
  if (code === 0) {
    return 0;
  }
  const clamped = Math.min(code, 0x1ff);
  let f = (10 << (clamped >> 7)) << 16;
  const frac = clamped & 0x7f;
  for (let i = 0; i < 7; i++) {
    if (frac & (0x40 >> i)) {
      f = ((f * OCT_FRAC[i]) + 0x8000) >> 16;
    }
  }
  return (f + 0x8000) >> 16;
}

export function hdFreq(raw, high) {
  const df = high ? FREQ_DEFAULT[1] : FREQ_DEFAULT[0];
  const limit = raw === 0 ? df : raw;
  return Math.max(FREQ_MIN, Math.min(limit, FREQ_MAX));
}

/** ns2_rumble_keys 镜像：状态字 bit4-5 声明有效子帧数，逐子帧解 40 位位串。 */
export function decodeKeys(raw16) {
  const keys = [];
  for (let g = 0; g < 3; g++) {
    // 40 位位串超出 JS 位运算的 32 位刻度，按 BigInt 解。
    let v = 0n;
    for (let i = 4; i >= 0; i--) {
      v = (v << 8n) | BigInt(raw16[1 + g * 5 + i]);
    }
    keys.push({
      lf_freq: keyFreqHz(Number(v & 0x1ffn)),
      lf_amp: Number((v >> 10n) & 0x3ffn),
      hf_freq: keyFreqHz(Number((v >> 20n) & 0x1ffn)),
      hf_amp: Number((v >> 32n) & 0xffn) << 2,
    });
  }
  const declared = (raw16[0] >> 4) & 0x3;
  return declared > 0 && declared < 3 ? keys.slice(0, declared) : keys;
}

/** 主机声明的子帧数（0 = 未声明，按满 3 处理）：轮播长度就是它。 */
export function declaredCount(sideKeys) {
  return sideKeys.length > 0 && sideKeys.length < 3 ? sideKeys.length : 3;
}

/** pad_feedback_hd_render 的子帧规则：有效子帧振幅 >>2 直迁、频率夹取回落，无效子帧静默。 */
export function hdRender(sideKeys) {
  const out = [];
  for (let k = 0; k < 3; k++) {
    if (k < sideKeys.length) {
      const src = sideKeys[k];
      const active = src.lf_amp !== 0 || src.hf_amp !== 0;
      out.push({
        lf_freq: active ? hdFreq(src.lf_freq, false) : 0,
        lf_gain: active ? src.lf_amp >> 2 : 0,
        hf_freq: active ? hdFreq(src.hf_freq, true) : 0,
        hf_gain: active ? src.hf_amp >> 2 : 0,
      });
    } else {
      out.push({ lf_freq: 0, lf_gain: 0, hf_freq: 0, hf_gain: 0 });
    }
  }
  return out;
}

/** pad_haptic_pulse_step 的镜像：返回 [幅度, 音高]。 */
export function pulseStep(steps, ageMs) {
  const period = steps.at(-1)[0];
  const ms = ageMs % period;
  for (const [until, amp, hz] of steps) {
    if (ms < until) {
      return [amp, amp ? hz : 0];
    }
  }
  return [0, 0];
}

/** 触觉增益倍率（A/B 标定用）：只改增益、夹回 0-255，频率与段边界不动。 */
export function scaledKey(key, gain) {
  const out = { ...key };
  out.lf_gain = Math.max(0, Math.min(255, Math.trunc(key.lf_gain * gain + 0.5)));
  out.hf_gain = Math.max(0, Math.min(255, Math.trunc(key.hf_gain * gain + 0.5)));
  return out;
}

/** 主机反馈状态机：吃采集记录，按时间给出「这一拍该渲染什么」。
 * hdGain 是触觉增益倍率（默认 1.0 = 与固件同刻度）。 */
export class FeedbackSim {
  constructor(records, hdGain = 1.0) {
    this.records = records;
    this.hdGain = hdGain;
    this.t0 = records[0][0];
    this.rumbleRaw = { 0: null, 1: null };
    /** 每侧主机声明的子帧数（声明之外的槽位不占时间）。 */
    this.keyCount = { 0: 3, 1: 3 };
    this.sample = null;
    this.sampleStart = null;
    this.sampleLast = null;
  }

  /** 该侧当前声明的子帧数。 */
  count(side) {
    return this.keyCount[side];
  }

  /** 推进到 nowMs（相对采集起点），返回 [keysBySide, speaker]。 */
  at(nowMs) {
    while (this.records.length && (this.records[0][0] - this.t0) * 1000.0 <= nowMs) {
      const [t, name, payload] = this.records.shift();
      if (name === 'rumble' && payload.length >= 33) {
        this.rumbleRaw = { 0: payload.subarray(1, 17), 1: payload.subarray(17, 33) };
        this.keyCount = {
          0: declaredCount(decodeKeys(this.rumbleRaw[0])),
          1: declaredCount(decodeKeys(this.rumbleRaw[1])),
        };
      } else if (name === 'composite' && payload.length >= 45) {
        const frame = payload.subarray(33);
        if (frame[0] === 0x0a && frame.length >= 9) {
          const sample = frame[3] === 0x02 ? frame[8] : frame[3];
          if (sample) {
            if (this.sample !== sample) {
              this.sampleStart = t;
            }
            this.sample = sample;
          } else {
            this.sample = null;
          }
          this.sampleLast = t;
        }
      }
    }

    const keys = {
      0: this.rumbleRaw[0] ? hdRender(decodeKeys(this.rumbleRaw[0])) : hdRender([]),
      1: this.rumbleRaw[1] ? hdRender(decodeKeys(this.rumbleRaw[1])) : hdRender([]),
    };
    let speaker = [0, 0];
    if (this.sample !== null && this.sampleLast !== null) {
      const age = (this.sampleLast - this.sampleStart) * 1000.0;
      const steps = this.sample === 0x02 ? LOCATE_STEPS
        : this.sample === 0x01 ? LF_BEEP_STEPS : DEFAULT_STEPS;
      if ((nowMs / 1000.0 - this.sampleLast) * 1000.0 <= HAPTIC_HOLD_MS) {
        const [amp, tone] = pulseStep(steps, age);
        if (amp === 0xc0) {
          // 强震段覆盖两侧音圈
          for (const side of [0, 1]) {
            keys[side] = Array.from({ length: 3 }, () => ({ lf_freq: 135, lf_gain: 255, hf_freq: 0, hf_gain: 0 }));
            this.keyCount[side] = 3;
          }
        } else if (amp === 0x80) {
          // 发声段铺扬声器
          speaker = [tone, 255];
        }
      }
    }
    if (this.hdGain !== 1.0) {
      for (const side of [0, 1]) {
        keys[side] = keys[side].map((key) => scaledKey(key, this.hdGain));
      }
    }
    return [keys, speaker];
  }
}

// ---------------------------------------------------------------- 落点

/** 按连接方式挑 DS5 的 gamepad 接口（usage 01/04 或 01/05）。
 * 连接方式看 HID 的 bus_type（1 = USB、2 = 蓝牙），不看 PID：0x0DF2 既是
 * DualSense 的蓝牙 PID，也是 DualSense Edge 的有线 PID；bus_type 缺失才退回 PID 判据。 */
export function findPad(conn, hid = deps.loadHid()) {
  // node-hid 3 的 devices() 只接受「无参或 vid+pid 双参」，这里无参枚举后自己过滤厂商。
  for (const info of hid.devices()) {
    if (info.vendorId !== 0x054c) {
      continue;
    }
    if (![0x0ce6, 0x0df2].includes(info.productId)) {
      continue;
    }
    if ((info.usagePage ?? 0) !== 0x01 || ![0x04, 0x05].includes(info.usage ?? 0)) {
      continue;
    }
    let isBt;
    if (info.busType === 'bluetooth') {
      isBt = true;
    } else if (info.busType === 'usb') {
      isBt = false;
    } else if (info.bus_type === 1 || info.bus_type === 2) {
      isBt = info.bus_type === 2;
    } else {
      isBt = info.productId === 0x0df2;
    }
    if ((conn === 'bt' && isBt) || (conn === 'usb' && !isBt)) {
      return info;
    }
  }
  return null;
}

/** 0x36 的喇叭帧要 Opus 编码器；缺失时同一路私有流退到 0x32。 */
export function opusAvailable() {
  try {
    new Bt36OpusEncoder();
  } catch {
    return false;
  }
  return true;
}

/** 解析 --pad 的实际落点，auto 跟产品固件的蓝牙通路保持一致。 */
export function resolvePad(choice, usbInfo, btInfo, opusAvailableFn = deps.opusAvailable) {
  if (choice !== 'auto') {
    return choice;
  }
  if (usbInfo !== null && usbInfo !== undefined) {
    return 'usb';
  }
  if (btInfo === null || btInfo === undefined) {
    return 'bt';
  }
  return opusAvailableFn() ? 'bt36' : 'bt32';
}

export function openHid(info, hid = deps.loadHid()) {
  return new hid.HID(info.path);
}

function crc32Of(data) {
  return crc32(data) >>> 0;
}

/** DS5 输出报告的公共段预置：喇叭音量钉 100、输出路径 0x30 = 手柄喇叭、前级 +6dB。 */
const PRESET_0X31 = { 2: 0x10, 3: 0xa3, 4: 0x90, 8: 100, 10: 0x30, 40: 0x02 };
const PRESET_0X02 = { 1: 0xa3, 2: 0x90, 6: 100, 8: 0x30, 38: 0x02 };

/** 蓝牙喇叭路由报告（0x31）：只写预置，不驱动马达。 */
export function buildSetup0x31(seq) {
  const out = Buffer.alloc(78);
  out[0] = 0x31;
  out[1] = (seq & 0xf) << 4;
  for (const [off, val] of Object.entries(PRESET_0X31)) {
    out[off] = val;
  }
  out.writeUInt32LE(crc32Of(Buffer.concat([Buffer.from([0xa2]), out.subarray(0, 74)])), 74);
  return out;
}

/** 有线喇叭路由与音量报告（0x02，48 字节，无 CRC）。 */
export function buildSetup0x02() {
  const out = Buffer.alloc(48);
  out[0] = 0x02;
  for (const [off, val] of Object.entries(PRESET_0X02)) {
    out[off] = val;
  }
  return out;
}

/** DS5 蓝牙 0x31 震动报告（预置同固件布局行，尾部 CRC 与固件 ps_bt_frame 同配方）。 */
export function buildRumble0x31(seq, left, right) {
  const out = Buffer.alloc(78);
  out[0] = 0x31;
  out[1] = (seq & 0xf) << 4;
  for (const [off, val] of Object.entries(PRESET_0X31)) {
    out[off] = val;
  }
  out[5] = right;
  out[6] = left;
  out.writeUInt32LE(crc32Of(Buffer.concat([Buffer.from([0xa2]), out.subarray(0, 74)])), 74);
  return out;
}

/** 固件 pad_rumble_perceived 的镜像：小档位直迁整段落进死区，回放落点要与
 * 固件写回看到同一个值，手感才有可比性。 */
export function perceivedAmp(amp) {
  if (amp <= 0) {
    return 0;
  }
  const value = 40 + 215.0 * Math.sqrt(amp / 255.0);
  return Math.max(53, Math.min(255, Math.trunc(value + 0.5)));
}

const sleepMs = (ms) => new Promise((resolve) => setTimeout(resolve, Math.max(0, ms)));

/** 蓝牙 0x31 落点：每 15ms 一拍，左大马达跟低频、右小马达跟高频，振幅过感知曲线。 */
export async function runBtHid(dev, sim, totalMs, speed) {
  console.log('蓝牙 0x31 HID 震动回放（HD 纹理压成两带马达，发声段无法渲染）');
  let seq = 0;
  const sent = [0, 0];
  let tick = 0;
  const durations = [];
  while (tick * 15.0 <= totalMs) {
    const [keys] = sim.at(tick * 15.0);
    const left = perceivedAmp(Math.max(0, ...keys[0].map((k) => k.lf_gain)));
    const right = perceivedAmp(Math.max(0, ...keys[1].map((k) => k.hf_gain)));
    const t0 = performance.now();
    dev.write(buildRumble0x31(seq, left, right));
    durations.push((performance.now() - t0) / 1000.0);
    seq = (seq + 1) & 0xf;
    sent[left || right ? 0 : 1] += 1;
    await sleepMs(15.0 / speed);
    tick += 1;
  }
  for (let i = 0; i < 3; i++) {
    dev.write(buildRumble0x31(seq, 0, 0));
    seq = (seq + 1) & 0xf;
  }
  console.log(`回放结束：震动 ${sent[0]} 拍、静默 ${sent[1]} 拍`);
  printReport(durations);
}

/** 回放前先写一份喇叭路由与音量档（0x31）。 */
function primeSpeakerRoute(dev) {
  try {
    dev.write(buildSetup0x31(0));
    return true;
  } catch (exc) {
    console.log(`喇叭路由报告写回失败（${exc.message}），喇叭可能不出声`);
    return false;
  }
}

function keyTuples(rendered) {
  return rendered.map((k) => [[k.lf_freq, k.lf_gain], [k.hf_freq, k.hf_gain]]);
}

function sideOf(count, keys) {
  return { count, keys: keyTuples(keys) };
}

/** 蓝牙 0x32 HD 落点：SAxense 142 字节原始形态直写，发声段折进音圈。 */
export async function runBt32(dev, sim, totalMs, speed) {
  console.log('蓝牙 0x32 私有触觉回放（142 字节原始形态，发声段折进音圈）');
  primeSpeakerRoute(dev);
  const state = new VoiceState();
  let seq = 0;
  const durations = [];
  let nextDue = performance.now() / 1000;
  let tickMs = 0.0;
  while (tickMs <= totalMs) {
    const [keys, speaker] = sim.at(tickMs);
    const pcm = btRenderPcm(sideOf(sim.count(0), keys[0]), sideOf(sim.count(1), keys[1]), [speaker], state);
    const t0 = performance.now();
    dev.write(btBuildReport(pcm, seq));
    durations.push((performance.now() - t0) / 1000.0);
    seq = (seq + 1) & 0xff;
    nextDue += BT_INTERVAL_S / speed;
    tickMs += BT_INTERVAL_S * 1000.0 * speed;
    const at = performance.now() / 1000;
    await sleepMs(Math.max(0, Math.min(nextDue - at, 0.05)) * 1000);
  }
  console.log('回放结束');
  printReport(durations);
}

/** 蓝牙 0x36 HD+喇叭落点：vds 398 字节形态，发声段由手柄喇叭真声播放。 */
export async function runBt36(dev, sim, totalMs, speed, encoder) {
  console.log('蓝牙 0x36 私有触觉+喇叭回放（HD 触觉 + 手柄喇叭真声）');
  primeSpeakerRoute(dev);
  const state = new VoiceState();
  const stateBeat = new VoiceState();
  let seq = 0;
  let packetSeq = 0;
  const durations = [];
  let nextDue = performance.now() / 1000;
  let tickMs = 0.0;
  while (tickMs <= totalMs) {
    const [keys, speaker] = sim.at(tickMs);
    const coil = btRenderPcm(sideOf(sim.count(0), keys[0]), sideOf(sim.count(1), keys[1]), [], state);
    const block = encoder.encode(renderSpeakerBeat(speaker.length ? speaker : [0, 0], stateBeat));
    const t0 = performance.now();
    dev.write(bt36BuildReport(coil, block, seq, packetSeq));
    durations.push((performance.now() - t0) / 1000.0);
    seq = (seq + 1) & 0xf;
    packetSeq = (packetSeq + 1) & 0xff;
    nextDue += BT_INTERVAL_S / speed;
    tickMs += BT_INTERVAL_S * 1000.0 * speed;
    const at = performance.now() / 1000;
    await sleepMs(Math.max(0, Math.min(nextDue - at, 0.05)) * 1000);
  }
  console.log('回放结束');
  printReport(durations);
}

/** 蓝牙 0x39 成对落点：一报 2 块触觉 + 2 帧喇叭，节拍 21.33ms。 */
export async function runBt39(dev, sim, totalMs, speed, encoder) {
  console.log('蓝牙 0x39 成对触觉+喇叭回放（一报 2 块，链路容差翻倍）');
  primeSpeakerRoute(dev);
  const state = new VoiceState();
  const stateBeat = new VoiceState();
  let seq = 0;
  let packetSeq = 0;
  const durations = [];
  let nextDue = performance.now() / 1000;
  let tickMs = 0.0;
  while (tickMs <= totalMs) {
    const [keys, speaker] = sim.at(tickMs);
    const coil = btRenderPcm(sideOf(sim.count(0), keys[0]), sideOf(sim.count(1), keys[1]), [], state, BT_FRAMES * 2);
    const block = encoder.encode(renderSpeakerPair(speaker.length ? speaker : [0, 0], stateBeat));
    const t0 = performance.now();
    dev.write(bt39BuildReport(coil, block, seq, packetSeq));
    durations.push((performance.now() - t0) / 1000.0);
    seq = (seq + 1) & 0xf;
    packetSeq = (packetSeq + 1) & 0xff;
    nextDue += BT39_INTERVAL_S / speed;
    tickMs += BT39_INTERVAL_S * 1000.0 * speed;
    const at = performance.now() / 1000;
    await sleepMs(Math.max(0, Math.min(nextDue - at, 0.05)) * 1000);
  }
  console.log('回放结束');
  printReport(durations);
}

/** USB 落点：WASAPI 4ch 流，哑渲染吃 HD 子帧与扬声器音色（全保真）。 */
export async function runUsb(sim, totalMs, speed) {
  let routeDev = null;
  const info = deps.findPad('usb');
  if (info !== null) {
    try {
      routeDev = deps.openHid(info);
      routeDev.write(buildSetup0x02());
    } catch (exc) {
      console.log(`路由/音量报告写回失败（${exc.message}），喇叭可能不出声`);
      if (routeDev !== null) {
        routeDev.close();
        routeDev = null;
      }
    }
  }
  const audio = new Ds5HapticsAudio();
  if (!(await audio.start())) {
    console.log('DualSense 音频端点打不开（被占用或不是直插 USB）');
    if (routeDev !== null) {
      routeDev.close();
    }
    return;
  }
  console.log('USB 音频触觉回放（4ch WASAPI，HD 全保真）');
  try {
    let tickMs = 0.0;
    while (tickMs <= totalMs) {
      const [keys, speaker] = sim.at(tickMs);
      audio.setParams({
        hd: {
          l: { count: sim.count(0), keys: keyTuples(keys[0]) },
          r: { count: sim.count(1), keys: keyTuples(keys[1]) },
          speaker,
        },
      });
      await sleepMs(5.0 / speed);
      tickMs += 5.0 * speed;
    }
  } finally {
    audio.stop();
    if (routeDev !== null) {
      routeDev.close();
    }
  }
  console.log('回放结束');
}

function cmdList() {
  console.log('手柄：');
  const hid = deps.loadHid();
  for (const info of hid.devices()) {
    if (info.vendorId !== 0x054c) {
      continue;
    }
    if ([0x0ce6, 0x0df2].includes(info.productId)) {
      const conn = info.productId === 0x0df2 ? '蓝牙' : 'USB ';
      console.log(`  DualSense 054c:${info.productId.toString(16).padStart(4, '0')} ${conn} `
                + `usage ${(info.usagePage ?? 0).toString(16).padStart(2, '0')}`
                + `/${(info.usage ?? 0).toString(16).padStart(2, '0')}`);
    }
  }
  console.log('样本：');
  const captures = readdirSync(SAMPLES_DIR).filter((name) => name.endsWith('.capture')).sort();
  for (const name of captures) {
    const rows = loadCapture(SAMPLES_DIR + name);
    const chans = {};
    for (const [, chan] of rows) {
      chans[chan] = (chans[chan] ?? 0) + 1;
    }
    const tail = Object.entries(chans).sort().map(([k, v]) => `${k}×${v}`).join(', ');
    console.log(`  ${name}: ${rows.length} 条（${tail}），${(rows.at(-1)[0] - rows[0][0]).toFixed(0)} 秒`);
  }
}

function printReport(durations) {
  if (!durations.length) {
    return;
  }
  const avg = (durations.reduce((a, b) => a + b, 0) / durations.length) * 1000.0;
  const mx = Math.max(...durations) * 1000.0;
  const slow = durations.filter((d) => d * 1000.0 > 10.67).length;
  console.log(`  （写回 ${durations.length} 份：平均 ${avg.toFixed(1)}ms、最大 ${mx.toFixed(1)}ms、超 10.67ms ${slow} 份）`);
}

/** Ctrl-C 信号量：收尾路径以 130 退出。 */
export class Interrupt extends Error {}

export async function main(argv = process.argv.slice(2)) {
  const program = new Command();
  program
    .name('pad_replay')
    .description('把 .capture 采集样本回放到 DualSense 手柄')
    .argument('[capture]', '要回放的 .capture 样本')
    .option('--pad <choice>', '回放落点：auto|usb|bt|bt32|bt36|bt39', 'auto')
    .option('--speed <x>', '回放速度倍率', Number, 1.0)
    .option('--hd-gain <x>', '触觉增益倍率（默认 4.0 = 布局行 hd 规则的标定值）', Number, 4.0)
    .option('--list', '列出手柄与样本后退出')
    .parse(argv, { from: 'user' });
  const args = program.opts();
  const capture = program.args[0];

  if (args.list) {
    cmdList();
    return 0;
  }
  try {
    if (!capture) {
      program.error('给一个 .capture 样本，或用 --list');
    }
    const records = loadCapture(SAMPLES_DIR + capture);
    const totalMs = (records.at(-1)[0] - records[0][0]) * 1000.0;
    const sim = new FeedbackSim(records, args.hdGain);
    console.log(`回放 ${capture}：${records.length} 条记录，${(totalMs / 1000.0).toFixed(0)} 秒，`
            + `${args.speed.toFixed(2)}x`);

    const conn = resolvePad(args.pad, deps.findPad('usb'), deps.findPad('bt'));
    if (args.pad === 'auto') {
      console.log(`auto 落点：${conn}`);
    }
    if (conn === 'usb') {
      await runUsb(sim, totalMs, args.speed);
      return 0;
    }
    const info = deps.findPad('bt');
    if (info === null) {
      console.log('没找到蓝牙连接的 DualSense');
      return 1;
    }
    const dev = deps.openHid(info);
    try {
      if (['bt32', 'bt36', 'bt39'].includes(conn)) {
        let encoder = null;
        if (conn === 'bt36' || conn === 'bt39') {
          try {
            encoder = new Bt36OpusEncoder();
          } catch (exc) {
            console.log(`0x36 喇叭流不可用（${exc.message}），回落 0x32`);
          }
        }
        try {
          if (encoder !== null) {
            if (conn === 'bt39') {
              await deps.runBt39(dev, sim, totalMs, args.speed, encoder);
            } else {
              await deps.runBt36(dev, sim, totalMs, args.speed, encoder);
            }
          } else {
            await deps.runBt32(dev, sim, totalMs, args.speed);
          }
        } catch (exc) {
          if (exc instanceof Interrupt) {
            throw exc;
          }
          // 私有流写回被拒：与产品路径同语义，回落 HID 震动写回。
          console.log(`私有流写回被拒（${exc.message}），回落 0x31 HID 震动`);
          await deps.runBtHid(dev, sim, totalMs, args.speed);
        }
      } else {
        await deps.runBtHid(dev, sim, totalMs, args.speed);
      }
    } finally {
      dev.close();
    }
  } catch (exc) {
    if (exc instanceof Interrupt) {
      // Ctrl-C 随时打断：各落点的 finally 已停音频流/关 HID 句柄，这里只收尾。
      console.log('\n已中断（Ctrl-C）');
      return 130;
    }
    throw exc;
  }
  return 0;
}

const isMain = process.argv[1] && import.meta.url === new URL(`file:///${process.argv[1].replace(/\\/g, '/')}`).href;
if (isMain) {
  main().then((code) => process.exit(code ?? 0), (exc) => {
    console.error(exc);
    process.exit(1);
  });
}
